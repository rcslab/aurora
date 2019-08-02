#include <sys/param.h>

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>

#include "slsmm.h"

#include "sls.h"
#include "sls_file.h"
#include "sls_mem.h"
#include "imported_sls.h"

/* TEMP */
uint64_t size_sent = 0;
/* XXX dynamically change to have size equal to one block */
char zeroblk[PAGE_SIZE];

int
sls_file_read(void *addr, size_t len, struct file *fp)
{
	int error = 0;

	struct uio auio;
	struct iovec aiov;
	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	/* 
	 * XXX The fd argument is bogus, but it's not needed
	 * except for ktrace. Both dofileread() and dofilewrite()
	 * can be split so that the fd-agnostic part can be called
	 * on its own.
	 *
	 * Also, dofileread/dofilewrite are static, so we "import" them
	 * by including them in the imports file.
	 */
	error = dofileread(curthread, 0, fp, &auio, (off_t) -1, 0);
	if (error != 0)
	    SLS_DBG("Error: dofileread failed with code %d\n", error);

	return error;
}

int
sls_file_write(void *addr, size_t len, struct file *fp)
{
	int error = 0;
	struct uio auio;
	struct iovec aiov;


	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	error = dofilewrite(curthread, 0, fp, &auio, (off_t) -1, 0);
	if (error != 0)
	    SLS_DBG("Error: kern_writev failed with code %d\n", error);

	return error;
}
int
sls_fd_read(void *addr, size_t len, int fd)
{
	int error = 0;

	struct uio auio;
	struct iovec aiov;
	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	error = kern_readv(curthread, fd, &auio);
	if (error != 0)
	    SLS_DBG("Error: kern_readv failed with code %d\n", error);

	return error;
}

int
sls_fd_write(void *addr, size_t len, int fd)
{
	int error = 0;
	struct uio auio;
	struct iovec aiov;


	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	error = kern_writev(curthread, fd, &auio);
	if (error != 0)
	    SLS_DBG("Error: kern_writev failed with code %d\n", error);

	return error;
}


int
file_writev(struct iovec *iov, size_t iovlen, int fd)
{
	int error = 0;
	struct uio auio;
	size_t size;
	int i;

	bzero(&auio, sizeof(struct uio));

	size = 0;
	for (i = 0; i < iovlen; i++)
	    size += iov[i].iov_len;


	auio.uio_iov = iov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_iovcnt = iovlen;
	auio.uio_resid = size;
	auio.uio_td = curthread;

	error = kern_writev(curthread, fd, &auio);
	if (error) {
		printf("Error: kern_writev failed with code %d\n", error);
	}

	return error;
}
