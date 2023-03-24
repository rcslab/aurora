#include <sys/param.h>
#include <sys/systm.h>
#include <sys/file.h>
#include <sys/syslimits.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <slos.h>
#include <slos_inode.h>

#include "sls_internal.h"
#include "sls_io.h"

int
slsio_open_vfs(char *name, int *fdp)
{
	struct thread *td = curthread;
	int error;

	error = kern_openat(
	    td, AT_FDCWD, name, UIO_SYSSPACE, O_CREAT | O_RDWR, S_IWUSR);
	if (error != 0)
		return (error);

	*fdp = td->td_retval[0];

	return (0);
}

int
slsio_fdread(int fd, char *buf, size_t len, off_t *offp)
{
	struct thread *td = curthread;
	struct uio auio;
	struct iovec aiov;
	int error;

	if (len > IOSIZE_MAX)
		return (EINVAL);

	aiov.iov_base = buf;
	aiov.iov_len = len;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;

	auio.uio_resid = len;
	auio.uio_segflg = UIO_SYSSPACE;

	if (offp != NULL)
		error = kern_preadv(td, fd, &auio, *offp);
	else
		error = kern_readv(td, fd, &auio);

	if (error == 0) {
		KASSERT(td->td_retval[0] == len,
		    ("read %ld, expected %ld", td->td_retval[0], len));
	}

	return (error);
}

int
slsio_fdwritev(int fd, struct iovec *aiov, size_t count, off_t *offp)
{
	/* XXX HACK. We normally compute uio_resid. */
	size_t len = count * PAGE_SIZE;
	struct thread *td = curthread;
	struct uio auio;
	int error;

	if (len > IOSIZE_MAX)
		return (EINVAL);

	auio.uio_iov = aiov;
	auio.uio_iovcnt = count;

	auio.uio_resid = len;
	auio.uio_segflg = UIO_SYSSPACE;

	if (offp != NULL)
		error = kern_pwritev(td, fd, &auio, *offp);
	else
		error = kern_writev(td, fd, &auio);

	if (error != 0)
		return (error);

	KASSERT(td->td_retval[0] == len,
	    ("wrote %ld, expected %ld", td->td_retval[0], len));
	return (0);
}

int
slsio_fdwrite(int fd, char *buf, size_t len, off_t *offp)
{
	struct thread *td = curthread;
	struct uio auio;
	struct iovec aiov;
	int error;

	if (len > IOSIZE_MAX)
		return (EINVAL);

	aiov.iov_base = buf;
	aiov.iov_len = len;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;

	auio.uio_resid = len;
	auio.uio_segflg = UIO_SYSSPACE;

	if (offp != NULL)
		error = kern_pwritev(td, fd, &auio, *offp);
	else
		error = kern_writev(td, fd, &auio);

	if (error != 0)
		return (error);

	KASSERT(td->td_retval[0] == len,
	    ("wrote %ld, expected %ld", td->td_retval[0], len));
	return (0);
}

/*
 * Open a SLOS inode as a file pointer for IO. The kern_openat() call
 * uses paths and allocates file descriptor table entries, and we want
 * neither of those things.
 */
int
slsio_open_sls(uint64_t oid, bool create, struct file **fpp)
{
	struct thread *td = curthread;
	int mode = FREAD | FWRITE;
	struct vnode *vp;
	struct file *fp;
	int error;

	if (create) {
		/* Try to create the node, if not already there, wrap it in a
		 * vnode. */
		error = slos_svpalloc(&slos, MAKEIMODE(VREG, S_IRWXU), &oid);
		if (error != 0)
			return (error);
	}

	/*
	 * Allocate a file structure. The descriptor to reference it
	 * is allocated and set by finstall() below.
	 */
	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (error);
	/*
	 * An extra reference on `fp' has been held for us by
	 * falloc_noinstall().
	 */

	/* Get the vnode for the record and open it. */
	error = VFS_VGET(slos.slsfs_mount, oid, LK_EXCLUSIVE, &vp);
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}

	/* Open the record for writing. */
	error = vn_open_vnode(vp, mode, td->td_ucred, td, fp);
	if (error != 0) {
		vput(vp);
		fdrop(fp, td);
		return (error);
	}

	/*
	 * Store the vnode, for any f_type. Typically, the vnode use
	 * count is decremented by direct call to vn_closefile() for
	 * files that switched type in the cdevsw fdopen() method.
	 */
	KASSERT(fp->f_ops == &badfileops, ("file methods already set"));
	fp->f_vnode = vp;
	fp->f_seqcount = 1;
	finit(fp, mode, DTYPE_VNODE, vp, &vnops);

	VOP_UNLOCK(vp, 0);

	*fpp = fp;
	/*
	 * Release our private reference, leaving the one associated with
	 * the descriptor table intact.
	 */
	return (0);
}

static int
slsio_doio(struct file *fp, void *buf, size_t len, enum uio_rw rw)
{
	struct thread *td = curthread;
	size_t iosize = 0;
	uint64_t back = 0;
	struct iovec aiov;
	struct uio auio;
	int error = 0;

	ASSERT_VOP_LOCKED(vp, ("vnode %p is unlocked", vp));

	aiov.iov_base = buf;
	aiov.iov_len = len;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;

	auio.uio_offset = -1;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = rw;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	/* If we don't want to do anything just return. */
	if (sls_drop_io)
		return (0);

	/* Do the IO itself. */
	iosize = auio.uio_resid;

	while (auio.uio_resid > 0) {
		back = auio.uio_resid;
		if (auio.uio_rw == UIO_WRITE) {
			error = fo_write(fp, &auio, td->td_ucred, 0, td);
		} else {
			error = fo_read(fp, &auio, td->td_ucred, 0, td);
		}
		if (error != 0) {
			goto out;
		}

		if (back == auio.uio_resid)
			break;
	}

	if (auio.uio_rw == UIO_WRITE)
		sls_bytes_written_vfs += iosize;
	else
		sls_bytes_read_vfs += iosize;
out:

	sls_io_initiated += 1;

	return (error);
}

int
slsio_fpread(struct file *fp, void *buf, size_t size)
{
	return (slsio_doio(fp, buf, size, UIO_READ));
}

int
slsio_fpwrite(struct file *fp, void *buf, size_t size)
{
	return (slsio_doio(fp, buf, size, UIO_WRITE));
}
