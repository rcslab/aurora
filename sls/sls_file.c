#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/selinfo.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/endian.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/tty.h>
#include <sys/un.h>
#include <sys/unistd.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

#include <machine/param.h>

/* XXX Pipe has to be after selinfo */
#include <sys/pipe.h>

/*
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <slos.h>
#include <sls_data.h>

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vnode.h"

SDT_PROBE_DEFINE0(sls, , , fileprobe_start);
SDT_PROBE_DEFINE1(sls, , , fileprobe_return, "int");

/*
 * Function vector and related boilerplate for the different file types.
 */

extern struct slsfile_ops slsvn_ops;
extern struct slsfile_ops slssock_ops;
extern struct slsfile_ops slspipe_ops;
extern struct slsfile_ops slskq_ops;
extern struct slsfile_ops slsshm_ops;
extern struct slsfile_ops slspts_ops;

static int
slsfile_slsid_invalid(struct file *fp, uint64_t *slsidp)
{
	return (EINVAL);
}

static int
slsfile_checkpoint_invalid(
    struct file *fp, struct sls_record *rec, struct slsckpt_data *sckpt_data)
{
	return (EINVAL);
}

static bool
slsfile_supported_invalid(struct file *fp)
{
	return (false);
}

static int
slsfile_restore_invalid(void *slsbacker, struct slsfile *info,
    struct slsrest_data *restdata, struct file **fpp)
{
	return (EINVAL);
}

static struct slsfile_ops slsfile_invalid = {
	.slsfile_supported = slsfile_supported_invalid,
	.slsfile_slsid = slsfile_slsid_invalid,
	.slsfile_checkpoint = slsfile_checkpoint_invalid,
	.slsfile_restore = slsfile_restore_invalid,
};

struct slsfile_ops *slsfile_ops[] = {
	&slsfile_invalid, /* DTYPE_NONE */
	&slsvn_ops,	  /* DTYPE_VNODE */
	&slssock_ops,	  /* DTYPE_SOCKET */
	&slspipe_ops,	  /* DTYPE_PIPE */
	&slsvn_ops,	  /* DTYPE_FIFO */
	&slskq_ops,	  /* DTYPE_CRYPTO */
	&slsfile_invalid, /* DTYPE_CRYPTO */
	&slsfile_invalid, /* DTYPE_MQUEUE */
	&slsshm_ops,	  /* DTYPE_SHM */
	&slsfile_invalid, /* DTYPE_SEM */
	&slspts_ops,	  /* DTYPE_PTS */
	&slsfile_invalid, /* DTYPE_DEV */
	&slsfile_invalid, /* DTYPE_PROCDESC */
	&slsfile_invalid, /* DTYPE_LINUXEFD */
	&slsfile_invalid, /* DTYPE_LINUXTFD */
};

static int
slsfile_slsid(struct file *fp, uint64_t *slsidp)
{
	return ((slsfile_ops[fp->f_type]->slsfile_slsid)(fp, slsidp));
}

static int
slsfile_checkpoint(
    struct file *fp, struct sls_record *rec, struct slsckpt_data *sckpt_data)
{
	return (
	    (slsfile_ops[fp->f_type]->slsfile_checkpoint)(fp, rec, sckpt_data));
}

static int
slsfile_restore(void *slsbacker, struct slsfile *finfo,
    struct slsrest_data *restdata, struct file **fpp)
{
	return ((slsfile_ops[finfo->type]->slsfile_restore)(
	    slsbacker, finfo, restdata, fpp));
}

static bool
slsfile_supported(struct file *fp)
{
	return (slsfile_ops[fp->f_type]->slsfile_supported)(fp);
}

static int
slsfile_is_ttyvp(struct file *fp)
{
	return (
	    (fp->f_type == DTYPE_VNODE) && (slsckpt_vnode_istty(fp->f_vnode)));
}

void
slsfile_attemptclose(int fd)
{
	struct thread *td = curthread;
	int error;

	/* Can't do much if we fail to close the file. */
	error = kern_close(td, fd);
	if (error != 0)
		DEBUG1("Error %d when closing file", error);
}

int
slsfile_extractfp(int fd, struct file **fpp)
{
	struct thread *td = curthread;
	struct file *fp = FDTOFP(td->td_proc, fd);

	/* Get a reference on behalf of the hashtable. */
	if (!fhold(fp))
		return (EBADF);

	/*
	 * Remove it from this process and this fd. If closing the fd fails
	 * we keep going, since we cannot handle the error meaningfully.
	 */
	slsfile_attemptclose(fd);
	*fpp = fp;

	return (0);
}

static int
slsfile_setinfo(struct file *fp, struct sls_record *rec, int type)
{
	struct slsfile info;
	int error;

	info.magic = SLSFILE_ID;
	info.slsid = rec->srec_id;
	info.type = type;
	info.flag = fp->f_flag;
	info.offset = fp->f_offset;
	info.vnode = (uint64_t)fp->f_vnode;

	error = sbuf_bcat(rec->srec_sb, (void *)&info, sizeof(info));
	if (error != 0)
		return (error);

	return (0);
}

/*
 * Get generic file info applicable to all kinds of files.
 * While one struct file can belong to multiple descriptor
 * tables, the underlying structures (pipes, sockets, etc.)
 * belong to exactly one open file when anonymous. That means
 * that we can safely store them together.
 */
static int __attribute__((noinline)) slsckpt_file(
    struct file *fp, uint64_t *fpslsid, struct slsckpt_data *sckpt_data)
{
	struct sls_record *rec;
	uint64_t slsid;
	int error;
	int type;

	/* Get the unique ID and create an empty record. */
	error = slsfile_slsid(fp, &slsid);
	if (error != 0)
		return (error);

	rec = sls_getrecord_empty(slsid, SLOSREC_FILE);

	/*
	 * Try to add the record into the table. If it is not inserted,
	 * it's already there and we're done.
	 */
	error = slskv_add(sckpt_data->sckpt_rectable, slsid, (uintptr_t)rec);
	if (error != 0) {
		sls_record_destroy(rec);
		*fpslsid = slsid;
		return (0);
	}

	/* We treat vnodes backed by TTYs as tty files. */
	type = (slsfile_is_ttyvp(fp)) ? DTYPE_PTS : fp->f_type;

	/* Set generic file info. */
	error = slsfile_setinfo(fp, rec, type);
	if (error != 0)
		goto error;

	/* Grab file type specific state. */
	error = slsfile_checkpoint(fp, rec, sckpt_data);
	if (error != 0)
		goto error;

	/* Seal the record to prevent further changes. */
	error = sls_record_seal(rec);
	if (error != 0)
		goto error;

	/* Propagage to the caller the ID we found. */
	*fpslsid = slsid;

	return (0);

error:
	/* Destroy the incomplete record. */
	slskv_del(sckpt_data->sckpt_rectable, slsid);
	sls_record_destroy(rec);

	return (error);
}

int
slsrest_file(
    void *slsbacker, struct slsfile *info, struct slsrest_data *restdata)
{
	struct thread *td = curthread;
	struct file *fp;
	int error;

	DEBUG1("Restoring file of type %d", info->type);
	error = slsfile_restore(slsbacker, info, restdata, &fp);
	if (error != 0)
		return (error);

	/*
	 * Sometimes we get no file pointer back. For example, we might support
	 * ignoring missing files, or when restoring an already created pipe
	 * end.
	 */
	if (fp == NULL)
		return (0);

	fp->f_flag = info->flag;
	fp->f_offset = info->offset;

	error = slskv_add(restdata->fptable, info->slsid, (uintptr_t)fp);
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}

	return (0);
}


int
slsckpt_filedesc(
    struct proc *p, struct slsckpt_data *sckpt_data, struct sbuf *sb)
{
	struct filedesc *fdp = p->p_fd;
	struct slsfiledesc *slsfdp;
	uint64_t fd_size;
	struct file *fp;
	int error = 0;
	int fd;


	error = slsckpt_vnode(fdp->fd_cdir, sckpt_data);
	if (error != 0)
		return (error);

	error = slsckpt_vnode(fdp->fd_rdir, sckpt_data);
	if (error != 0)
		return (error);

	/* The end of the struct is a variable length array. */
	fd_size = sizeof(*slsfdp) * sizeof(uint64_t) * (fdp->fd_lastfile - 1);
	slsfdp = malloc(fd_size, M_SLSMM, M_WAITOK | M_ZERO);

	slsfdp->cdir = (uint64_t)fdp->fd_cdir;
	slsfdp->rdir = (uint64_t)fdp->fd_rdir;
	slsfdp->fd_cmask = fdp->fd_cmask;
	slsfdp->fd_lastfile = fdp->fd_lastfile;
	slsfdp->magic = SLSFILEDESC_ID;

	FILEDESC_XLOCK(fdp);

	/* Scan the entries of the table for active file descriptors. */
	for (fd = 0; fd <= fdp->fd_lastfile; fd++) {
		if (!fdisused(fdp, fd)) {
			continue;
		}

		DEBUG1("Checkpointing fd %d", fd);
		KASSERT((fdp == p->p_fd), ("wrong file descriptor table"));
		fp = FDTOFP(p, fd);

		/* Check if the file is currently supported by SLS. */
		if (!slsfile_supported(fp)) {
			continue;
		}

		/* Checkpoint the file structure itself. */
		SDT_PROBE0(sls, , , fileprobe_start);
		error = slsckpt_file(fp, &slsfdp->fd_table[fd], sckpt_data);
		SDT_PROBE1(sls, , , fileprobe_return, fp->f_type);
		if (error != 0)
			goto done;
	}

	/* Add the size of the struct before the struct itself. */
	error = sbuf_bcat(sb, (void *)&fd_size, sizeof(fd_size));
	if (error != 0)
		goto done;

	error = sbuf_bcat(sb, (void *)slsfdp, fd_size);
	if (error != 0)
		goto done;

done:
	FILEDESC_XUNLOCK(fdp);

	free(slsfdp, M_SLSMM);

	return (error);
}

int
slsrest_filedesc(
    struct proc *p, struct slsfiledesc *info, struct slsrest_data *restdata)
{
	struct thread *td = curthread;
	struct filedesc *newfdp;
	int stdfds[] = { 0, 1, 2 };
	struct vnode *cdir, *rdir;
	struct file *fp;
	uint64_t slsid;
	int error = 0;
	uint64_t fd;
	int res;

	error = slskv_find(restdata->vntable, info->cdir, (uintptr_t *)&cdir);
	if (error != 0)
		return (EINVAL);

	error = slskv_find(restdata->vntable, info->rdir, (uintptr_t *)&rdir);
	if (error != 0)
		return (EINVAL);

	/*
	 * Create the new file descriptor table. It has
	 * the same size as the old one, but it's empty.
	 */
	error = fdcopy_remapped(p->p_fd, stdfds, 0, &newfdp);
	if (error != 0)
		return (error);
	fdinstall_remapped(curthread, newfdp);

	/* Go into the new root and set it as such. */
	VOP_LOCK(rdir, LK_EXCLUSIVE);
	error = pwd_chroot(td, rdir);
	VOP_UNLOCK(rdir, 0);
	if (error != 0)
		return (error);

	/* Set the current directory. */
	VOP_LOCK(cdir, LK_EXCLUSIVE);
	error = change_dir(cdir, td);
	VOP_UNLOCK(cdir, 0);
	if (error != 0)
		return (error);

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info->fd_cmask;

	/* Attach the appropriate open files to the descriptor table. */
	for (fd = 0; fd <= info->fd_lastfile; fd++) {
		slsid = info->fd_table[fd];
		if (slsid == 0)
			continue;

		/* Get the restored open file from the ID. */
		error = slskv_find(restdata->fptable, slsid, (uint64_t *)&fp);
		if (error != 0)
			goto error;

		/* We restore the file _exactly_ at the same fd.*/
		error = fdalloc(curthread, fd, &res);
		if (error != 0)
			goto error;

		if (res != fd) {
			error = EINVAL;
			goto error;
		}

		/* Get a reference to the open file for the table and install
		 * it. */
		if (!fhold(fp)) {
			error = EBADF;
			goto error;
		}

		/*
		 * XXX Keep the UF_EXCLOSE flag with the entry somehow,
		 * maybe using bit ops? Then again, O_CLOEXEC is most
		 * often set in the kernel by looking at the type of the
		 * file being opened. The sole exception is vnode-backed
		 * files, which seem to be able to be both.
		 */
		_finstall(p->p_fd, fp, fd, O_CLOEXEC, NULL);

		/* Fix up the kqueue to point to its new fptable. */
		if (fp->f_type == DTYPE_KQUEUE)
			slskq_attach_locked(p, (struct kqueue *)fp->f_data);
	}

	FILEDESC_XUNLOCK(newfdp);

	return (0);

error:

	FILEDESC_XUNLOCK(newfdp);

	return (error);
}
