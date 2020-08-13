#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/param.h>

#include <sys/cdefs.h>
#include <sys/conf.h>
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
#include <sys/tty.h>
#include <sys/unistd.h>
#include <sys/un.h>
#include <sys/unpcb.h>
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

static int
sls_vn_to_inode(struct vnode *vp, uint64_t *inop)
{
	/* Get the inode number of the file through the vnode. */

	if (vp->v_mount != slos.slsfs_mount) 
		panic("unlinked vnode %p not in the SLOS", vp);

	SLS_KTR1("Got inum %lx from the SLOS", INUM(SLSVP(vp)));

	*inop = INUM(SLSVP(vp));

	SLS_KTR1("Checkpointing vnode backed by inode %lx", *inop);

	return (0);

}

/*
 * Restore a vnode from a VFS path.
 */
static int
slsrest_path(struct sbuf *path, struct slsfile *info, int *fdp)
{
	int error;
	int fd;

	/* XXX Permissions/flags. */
	error = kern_openat(curthread, AT_FDCWD, sbuf_data(path),
	    UIO_SYSSPACE, O_RDWR, S_IRWXU);
	if (error != 0) {
		printf("ERR\n");
		return (error);
	}

	fd = curthread->td_retval[0];


	/* Export the fd to the caller. */
	*fdp = fd;

	return (0);
}

/*
 * Restore a vnode from a SLOS inode. This is mostly code from kern_openat()
 */
static int
slsrest_inode(struct slsfile *info, int *fdp)
{
	struct thread *td = curthread;
	struct filecaps *fcaps = NULL;
	int flags = FREAD | FWRITE;
	struct vnode *vp;
	struct file *fp;
	int indx = -1;
	int error;

	KASSERT(info->has_path == 0, ("restoring linked file by inode number"));

	SLS_KTR1("Restoring vnode backed by inode %lx", info->ino);

	/* Get an empty struct file. */
	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (error);

	/* Try to get the vnode from the SLOS. */
	error = VFS_VGET(slos.slsfs_mount, info->ino, LK_EXCLUSIVE, &vp);
	if (error != 0)
		goto error;

	fp->f_flag = FFLAGS(O_RDWR) & FMASK;

	error = vn_open_vnode(vp, FREAD | FWRITE, td->td_ucred, td, fp);
	if (error != 0) {
		vput(vp);
		goto error;
	}

	/* XXX We don't know what to do with anonymous FIFOs just yet. */
	KASSERT(vp->v_type != VFIFO, ("Unexpected FIFO"));

	/* Set up the file struct. */
	fp->f_seqcount = 1;
	finit(fp, (flags & FMASK) | (fp->f_flag & FHASLOCK),
	    DTYPE_VNODE, vp, &vnops);

	VOP_UNLOCK(vp, 0);

	fp->f_vnode = vp;

	/* Add the new file struct to the table. */
	error = finstall(td, fp, &indx, flags, fcaps);
	if (error != 0)
		goto error;

	/* Export the fd to the caller. */
	*fdp = indx;

	/* Drop this thread's reference to the file struct. */
	fdrop(fp, td);

	return (0);

error:
	/* Drop the file table's reference. */
	if (indx != -1)
		kern_close(td, indx);

	/* Remove the reference from the SLS itself. */
	fdrop(fp, td);

	return (error);

}

int
slsckpt_vnode(struct proc *p, struct vnode *vp, struct slsfile *info, struct sbuf *sb)
{
	char *freepath = NULL;
	char *fullpath = "";
	size_t len;
	int error;

	vref(vp);
	error = vn_fullpath(curthread, vp, &fullpath, &freepath);
	vrele(vp);
	switch(error) {
	case 0:
		/* Successfully found a path, go on with . */
		break;

	case ENOENT:
		/* File not in the VFS, try to get its location in the SLOS. */
		free(freepath, M_TEMP);

		error = sls_vn_to_inode(vp, &info->ino);
		if (error != 0)
			return (error);

		info->has_path = 0;

		/* Write out the struct file. */
		error = sbuf_bcat(sb, (void *) info, sizeof(*info));
		if (error != 0)
			return (error);

		return (0);

	default:
		/* Miscellaneous error. */
		goto error;
	}

	info->has_path = 1;
	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
		goto error;


	len = strnlen(fullpath, PATH_MAX);
	error = sls_path_append(fullpath, len, sb);
	if (error != 0)
		goto error;

	free(freepath, M_TEMP);

	return (0);

error:

	free(freepath, M_TEMP);
	return (error);
}

int
slsrest_vnode(struct sbuf *path, struct slsfile *info, int *fdp, int seekable)
{
	struct thread *td = curthread;
	int error, close_error;

	/* Get the vnode either from the inode or the path. */
	if (info->has_path == 1)
		error = slsrest_path(path, info, fdp);
	else
		error = slsrest_inode(info, fdp);
	if (error != 0)
		return (error);

	if (seekable == 0)
		return (0);

	/* Vnodes might be seekable. Fix up the offset here. */
	error = kern_lseek(td, *fdp, info->offset, SEEK_SET);
	if (error != 0) {
		close_error = kern_close(td, *fdp);
		if (close_error != 0)
			SLS_DBG("error %d when closing fd %d", close_error, *fdp);
		return error;
	}

	return (0);
}


int
slsckpt_fifo(struct proc *p, struct vnode *vp, struct slsfile *info, struct sbuf *sb)
{
	return slsckpt_vnode(p, vp, info, sb);
}

int
slsrest_fifo(struct sbuf *path, struct slsfile *info, int *fdp)
{
	return slsrest_vnode(path, info, fdp, 0);
}
