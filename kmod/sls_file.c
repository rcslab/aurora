#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/param.h>

#include <sys/domain.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

/* XXX Pipe has to be after selinfo */
#include <sys/pipe.h>

/* 
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <slos.h>
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_path.h"

#include "imported_sls.h"

/* XXX Spin out vnode operations to a new file. */

/* -------------- VNODE OPERATIONS -------------- */

/* Get the name of a vnode. This is the only information we need about it. */
static int
slsckpt_vnode(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	int error;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(vp, sb);
	PROC_LOCK(p);

	/* 
	 * XXX Make sure the unlinked file case is handled. 
	 */
	if (error == ENOENT)
	    return (0);

	return (0);
}

static int
slsrest_vnode(struct sbuf *path, struct slsfile *info, int *fdp)
{
	char *filepath;
	int error;
	int fd;

	filepath = sbuf_data(path);

	/* XXX Permissions/flags. Also, is O_CREAT reasonable? */
	error = kern_openat(curthread, AT_FDCWD, filepath, 
		UIO_SYSSPACE, O_RDWR | O_CREAT, S_IRWXU);	
	if (error != 0)
	    return (error);

	fd = curthread->td_retval[0];

	/* Vnodes are seekable. Fix up the offset here. */
	error = kern_lseek(curthread, fd, info->offset, SEEK_SET);
	if (error != 0)
	    return (error);

	/* Export the fd to the caller. */
	*fdp = fd;

	return (0);
}

/* -------------- END VNODE OPERATIONS -------------- */

static int
slsckpt_getvnode(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_vnode;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_vnode(p, fp->f_vnode, sb);
	if (error != 0)
	    return (error);

	return (0);
}

static int
slsckpt_getkqueue(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	struct kqueue *kq;
	int error;

	/* Acquire the kqueue for reading. */
	error = kqueue_acquire(fp, &kq);
	if (error != 0)
	    return (error);

	info->backer = (uint64_t) kq;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_kqueue(p, kq, sb);
	if (error != 0)
	    goto out;

	kqueue_release(kq, 0);

	return (0);

out:
	kqueue_release(kq, 0);
	return (error);
}

static int
slsckpt_getpipe(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_data;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_pipe(p, fp, sb);
	if (error != 0)
	    return (error);

	return (0);
}

static int
slsckpt_getsocket(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_data;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_socket(p, (struct socket *) fp->f_data, sb);
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
static int
slsckpt_file(struct proc *p, struct file *fp, uint64_t *slsid)
{
	struct slsfile info;
	struct sbuf *sb;
	int error;

	/* If we have already checkpointed this open file, return success. */
	if (slskv_find(slsm.slsm_rectable, (uint64_t) fp, (uintptr_t *) &sb) == 0)
	    return 0;

	info.type = fp->f_type;
	info.flag = fp->f_flag;
	info.offset = fp->f_offset;

	info.magic = SLSFILE_ID;
	*slsid = (uint64_t) fp;
	info.slsid = *slsid;

	sb = sbuf_new_auto();	

	/* Checkpoint further information depending on the file's type. */
	switch (fp->f_type) {
	/* Backed by a vnode - get the name. */
	case DTYPE_VNODE:
	    error = slsckpt_getvnode(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	/* Backed by a kqueue - get all pending knotes. */
	case DTYPE_KQUEUE:
	    error = slsckpt_getkqueue(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	/* Backed by a pipe - get existing data. */
	case DTYPE_PIPE:

	    /* 
	     * In order to be able to use the open file table 
	     * to find if we need to restore a file's peer, we
	     * need to make it so that the open file and its 
	     * backing pipe have the same ID. Pass it to the caller.
	     */
	    *slsid = (uint64_t) fp->f_data;
	    info.slsid = *slsid;

	    /* 
	     * Since we're using the pipe pointer instead of the file 
	     * pointer for deduplication, recheck whether we actually
	     * need to checkpoint.
	     */
	    if (slskv_find(slsm.slsm_rectable, (uint64_t) slsid, (uintptr_t *) &sb) == 0) {
		sbuf_delete(sb);
		return 0;
	    }

	    error = slsckpt_getpipe(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	case DTYPE_SOCKET:
	    error = slsckpt_getsocket(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	default:
	    panic("invalid file type");
	}

	/* Add the backing entities to the global tables. */
	error = slskv_add(slsm.slsm_rectable, *slsid, (uintptr_t) sb);
	if (error != 0)
	    return (error);

	error = slskv_add(slsm.slsm_typetable, (uint64_t) sb, (uintptr_t) SLOSREC_FILE);
	if (error != 0)
	    return (error);

	/* Give the SLS ID we want to use for the file pointer to the caller. */
	*slsid = info.slsid;

	return (0);

error:
	SLS_DBG("error %d", error);

	sbuf_delete(sb);

	return (error);

}

int 
slsrest_file(void *slsbacker, struct slsfile *info, 
	struct slskv_table *filetable, struct slskv_table *kqtable)
{
	struct file *fp;
	uintptr_t pipe;
	uint64_t slsid;
	void *kqdata;
	int error;
	void *kq;
	int fd;

	slsid = info->slsid;

	switch(info->type) {
	case DTYPE_VNODE:
	    error = slsrest_vnode((struct sbuf *) slsbacker, info, &fd);
	    if (error != 0)
		return (error);
	    break;

	case DTYPE_KQUEUE:
	    kqdata = ((slsset *) slsbacker)->data;
	    error = slsrest_kqueue((struct slskqueue *) kqdata, &fd);
	    if (error != 0)
		return (error);

	    /* 
	     * Associate the restored kqueue with its record. We can't 
	     * restore the kevents properly because we need to have a 
	     * fully restored file descriptor table. We therefore keep
	     * the set of kevents for the kqueue in a table until we need it.
	     */
	    kq = curproc->p_fd->fd_files->fdt_ofiles[fd].fde_file->f_data;
	    error = slskv_add(kqtable, (uint64_t) kq, (uintptr_t) slsbacker);
	    if (error != 0)
		return (error);

	    break;
	
	case DTYPE_PIPE:

	    /* 
	     * Pipes are a special case, because restoring one end 
	     * also brings back the other. For this reason, we look
	     * for the pipe's SLS ID instead of the open file's.
	     *
	     * Because the rest of the filetable's data is indexed
	     * by the ID held in the slsfile structure, we do an
	     * extra check using the ID of slspipe. If we find it,
	     * we have already restored the peer, and so we don't
	     * need to do anything.
	     */
	    slsid = ((struct slspipe *) slsbacker)->slsid;
	    if (slskv_find(filetable, slsid, &pipe) == 0)
		return 0;

	    error = slsrest_pipe(filetable, (struct slspipe *) slsbacker, &fd);
	    if (error != 0)
		return (error);
	    break;

	case DTYPE_SOCKET:
	    error = slsrest_socket(slsbacker, info, &fd);
	    if (error != 0)
		return (error);
	    break;

	default:
	    panic("invalid file type");
	}

	/* Get the open file from the table and add it to the hashtable. */
	fp = curthread->td_proc->p_fd->fd_files->fdt_ofiles[fd].fde_file;

	error = slskv_add(filetable, info->slsid, (uintptr_t) fp);
	if (error != 0) {
	    kern_close(curthread, fd);
	    return (error);
	}

	/* We keep the open file in the filetable, so we grab a reference. */
	fhold(fp);

	/* 
	 * Remove the open from this process' table. 
	 * When we find out we need it, we'll put
	 * it into the table of the appropriate process.
	 */
	kern_close(curthread, fd);

	return (0);
}

int
slsckpt_filedesc(struct proc *p, struct sbuf *sb)
{
	struct slsfiledesc slsfiledesc;
	struct slskv_table *fdtable;
	struct filedesc *filedesc;
	uint64_t slsid;
	struct socket *so;
	struct file *fp;
	int error = 0;
	int fd;

	error = slskv_create(&fdtable, SLSKV_NOREPLACE);
	if (error != 0)
	    return (error);

	filedesc = p->p_fd;

	vhold(filedesc->fd_cdir);
	vhold(filedesc->fd_rdir);

	slsfiledesc.fd_cmask = filedesc->fd_cmask;
	slsfiledesc.magic = SLSFILEDESC_ID;

	FILEDESC_XLOCK(filedesc);

	error = sbuf_bcat(sb, (void *) &slsfiledesc, sizeof(slsfiledesc));
	if (error != 0)
	    goto done;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(filedesc->fd_cdir, sb);
	PROC_LOCK(p);
	if (error) {
	    SLS_DBG("Error: cdir sls_vn_to_path failed with code %d\n", error);
	    goto done;
	}

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(filedesc->fd_rdir, sb);
	PROC_LOCK(p);
	if (error) {
	    SLS_DBG("Error: rdir sls_vn_to_path failed with code %d\n", error);
	    goto done;
	}

	for (fd = 0; fd <= filedesc->fd_lastfile; fd++) {
	    if (!fdisused(filedesc, fd))
		continue;

	    fp = filedesc->fd_files->fdt_ofiles[fd].fde_file;

	    /* 
	     * XXX Right now we checkpoint a subset of all file types. 
	     * Depending on what type the file is, put in some extra
	     * checks to see if we should actually checkpoint it.
	     */
	    switch (fp->f_type) {
	    case DTYPE_VNODE:
		/* Handle only regular vnodes for now. */
		if (fp->f_vnode->v_type != VREG)
		    continue;
		break;

	    case DTYPE_KQUEUE:
		/* Kqueues are fine. */
		break;

	    case DTYPE_PIPE:
		/* Pipes are also fine. */
		break;
	    case DTYPE_SOCKET:
		so = (struct socket *) fp->f_data;

		/* Only restore listening IPv4 TCP sockets. */
		if (so->so_type != SOCK_STREAM)
		    continue;

		if (so->so_proto->pr_protocol != IPPROTO_TCP)
		    continue;

		if (so->so_proto->pr_domain->dom_family != AF_INET)
		    continue;

		break;

	    default:
		continue;

	    }
	    /* Checkpoint the file structure itself. */
	    error = slsckpt_file(p, fp, &slsid);
	    if (error != 0)
		goto done;

	    /* Add the fd - slsid pair to the table. */
	    error = slskv_add(fdtable, fd, (uintptr_t) slsid);
	    if (error != 0)
		goto done;

	}

	/* Add the table to the descriptor table record. */
	error = slskv_serial(fdtable, sb);
	if (error != 0)
	    goto done;

done:

	vdrop(filedesc->fd_cdir);
	vdrop(filedesc->fd_rdir);

	FILEDESC_XUNLOCK(filedesc);

	slskv_destroy(fdtable);

	return (error);
}

int
slsrest_filedesc(struct proc *p, struct slsfiledesc info, 
	struct slskv_table *fdtable, struct slskv_table *filetable)
{
	int stdfds[] = { 0, 1, 2 };
	struct filedesc *newfdp;
	char *cdir, *rdir;
	struct file *fp;
	uint64_t slsid;
	int error = 0;
	int fd, res;

	cdir = sbuf_data(info.cdir);
	rdir = sbuf_data(info.rdir);
	/* 
	 * We assume that fds 0, 1, and 2 are still stdin, stdout, stderr.
	 * This is a valid assumption considering that we control the process 
	 * on top of which we restore.
	 *
	 * XXX Find a nicer way to do that. Some processes don't have these,
	 * how are we going to handle them? This is especially important for
	 * multiprocess checkpointing, where we may have multiple processes
	 * under one controlling terminal.
	 */
	error = fdcopy_remapped(p->p_fd, stdfds, 3, &newfdp);
	if (error != 0)
	    return (error);
	fdinstall_remapped(curthread, newfdp);

	PROC_UNLOCK(p);
	error = kern_chdir(curthread, cdir, UIO_SYSSPACE);
	if (error != 0)
	    return (error);

	error = kern_chroot(curthread, rdir, UIO_SYSSPACE);
	if (error != 0)
	    return (error);
	PROC_LOCK(p);

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info.fd_cmask;
	FILEDESC_XUNLOCK(newfdp);


	/* Attach the appropriate open files to the descriptor table. */
	while (slskv_pop(fdtable, (uint64_t *) &fd, (uintptr_t *) &slsid) == 0) {
	    /* Get the restored open file from the ID. */
	    error = slskv_find(filetable, slsid, (uint64_t *) &fp);
	    if (error != 0)
		return (error);

	    /* We restore the file _exactly_ at the same fd.*/
	    error = fdalloc(curthread, fd, &res);
	    if (error != 0)
		return (error);

	    if (res != fd)
		return (error);

	    /* Get a reference to the open file for the table and install it. */
	    fhold(fp);
	    /* XXX Keep the UF_EXCLOSE flag with the entry somehow, maybe w/ bit ops? */
	    _finstall(p->p_fd, fp, fd, O_CLOEXEC, NULL);
	}
	
	slskv_destroy(fdtable);

	return (0);
}
