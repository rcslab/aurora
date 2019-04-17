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

/* TEMP */
uint64_t size_sent = 0;
/* XXX dynamically change to have size equal to one block */
char zeroblk[PAGE_SIZE];

int
file_read(void* addr, size_t len, int fd)
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
	if (error) {
		printf("Error: kern_readv failed with code %d\n", error);
	}

	return error;
}

int
file_write(void* addr, size_t len, int fd)
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
	if (error) {
		printf("Error: kern_writev failed with code %d\n", error);
	}
	sls_log(7, len);

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
	sls_log(7, size);

	return error;
}

/*
 * XXX Things to improve in this group of read/write ops for the OSD:
 *
 * - Get the vp of the OSD from the mbmp (trivial). As a note,
 *   the mbmp struct is turning into an "in-memory metadata for the OSD" struct,
 *   not just a wrapper around the bitmap.
 *
 * - Actually use the VOP_* operations. Write now we are hacking through the issues
 *   by using an always-on module-wide fd for the OSD. We should change this 
 *   when we solve the -EIO error we get when we use these methods.
 *
 */
static int 
osd_preadv_aligned(struct osd_mbmp *mbmp, uint64_t block, 
	struct iovec *iov, size_t iovcnt, size_t len)
{
	struct uio auio;
	size_t blksize;

	if (iovcnt > UIO_MAXIOV / 2)
	    return EINVAL;

	bzero(&auio, sizeof(auio));
	/* XXX Align to block size later, not page size */
	blksize = PAGE_SIZE;
	if (len % blksize != 0)
	    return EINVAL;

	auio.uio_iov = iov;
	auio.uio_iovcnt = iovcnt;
	auio.uio_offset = block * blksize;
	auio.uio_resid = len;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_td = curthread;

	/* HACK */
	return kern_preadv(curthread, osdfd, &auio, block * blksize);
	//error = VOP_READ(mbmp->mbmp_osdvp, &auio, IO_NODELOCKED | IO_DIRECT, curthread->td_proc->p_ucred);
}

static void *
osd_padiov(size_t blksize, struct iovec *iov, size_t *iovcnt, size_t *padlen)
{
	size_t totallen, remlen;
	size_t newiovcnt;
	struct iovec *newiov;
	int i;

	newiov = malloc(sizeof(*newiov) * UIO_MAXIOV, M_SLSMM, M_WAITOK | M_ZERO);
	
	/* Get total size of the operation */
	newiovcnt = 0;
	totallen = 0;
	for (i = 0; i < *iovcnt; i++) {
	    memcpy(&newiov[newiovcnt++], &iov[i], sizeof(iov[i]));
	    totallen += iov[i].iov_len;

	    remlen = iov[i].iov_len % blksize; 
	    if (remlen != 0) {
		newiov[newiovcnt].iov_base = (void *) zeroblk;
		newiov[newiovcnt++].iov_len = blksize - remlen;
		totallen += blksize - remlen;
	    }

	}

	*padlen = totallen;
	*iovcnt = newiovcnt;

	return newiov;
}


int
osd_preadv(struct osd_mbmp *mbmp, uint64_t block, struct iovec *iov, size_t iovcnt)
{
	size_t padlen; 
	size_t blksize;
	struct iovec *newiov;
	size_t newiovcnt;
	int error = 0;

	blksize = PAGE_SIZE;
	newiovcnt = iovcnt;
	/* HACK */
	padlen = 0;
	for (int i = 0; i < iovcnt; i++)
	    padlen += iov[i].iov_len;
	newiovcnt = iovcnt;
	newiov = iov;
	//newiov = osd_padiov(blksize, iov, &iovcnt, &padlen);

	error = osd_preadv_aligned(mbmp, block, newiov, newiovcnt, padlen); 
	if (error != 0) {
	    printf("Error: osd_preadv_aligned failed with %d\n", error);
	}

	/* XXX Free iovcetor? */

	return error;
}

int
osd_pread(struct osd_mbmp *mbmp, uint64_t block, void *addr, size_t len)
{
	struct iovec *aiov;

	aiov = malloc(sizeof(*aiov) * UIO_MAXIOV, M_SLSMM, M_WAITOK | M_ZERO);

	aiov[0].iov_base = (void *) addr;
	aiov[0].iov_len = len;

	/* Is it safe to use an iovec allocated in the stack? */
	return osd_preadv(mbmp, block, aiov, 1);
}

static int 
osd_pwritev_aligned(struct osd_mbmp *mbmp, uint64_t block, 
	struct iovec *iov, size_t iovcnt, size_t len)
{
	struct uio auio;
	size_t blksize;

	bzero(&auio, sizeof(auio));
	/* XXX Align to block size later, not page size */
	blksize = PAGE_SIZE;
	if (len % blksize != 0)
	    return EINVAL;

	auio.uio_iov = iov;
	auio.uio_iovcnt = iovcnt;
	auio.uio_offset = block * blksize;
	auio.uio_resid = len;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_td = curthread;
	size_sent += len;

	/* HACK */
	return kern_pwritev(curthread, osdfd, &auio, block * blksize);
	//error = VOP_WRITE(mbmp->mbmp_osdvp, &auio, IO_NODELOCKED | IO_DIRECT, curthread->td_proc->p_ucred);
}

int
osd_pwritev(struct osd_mbmp *mbmp, uint64_t block, struct iovec *iov, size_t iovcnt)
{
	size_t padlen; 
	size_t blksize;
	struct iovec *newiov;
	size_t newiovcnt;
	int error = 0;

	blksize = PAGE_SIZE;
	newiovcnt = iovcnt;
	/* HACK */
	padlen = 0;
	for (int i = 0; i < iovcnt; i++)
	    padlen += iov[i].iov_len;
	newiovcnt = iovcnt;
	newiov = iov;
	//newiov = osd_padiov(blksize, iov, &iovcnt, &padlen);

	error = osd_pwritev_aligned(mbmp, block, newiov, newiovcnt, padlen); 
	if (error != 0) {
	    printf("Error: osd_pwritev_aligned failed with %d\n", error);
	}

	/* XXX Free iovector? */

	return error;
}


int
osd_pwrite(struct osd_mbmp *mbmp, uint64_t block, void *addr, size_t len)
{
	struct iovec *aiov;

	aiov = malloc(sizeof(*aiov) * UIO_MAXIOV, M_SLSMM, M_WAITOK | M_ZERO);

	aiov[0].iov_base = (void *) addr;
	aiov[0].iov_len = len;

	/* Is it safe to use an iovec allocated in the stack? */
	return osd_pwritev(mbmp, block, aiov, 1);
}
