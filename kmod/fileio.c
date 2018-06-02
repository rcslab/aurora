#include "fileio.h"

#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/uio.h>

int
fd_read(void* addr, size_t len, int fd)
{
    int error = 0;

    struct uio auio;
    struct iovec aiov;
    bzero(&aiov, sizeof(struct uio));
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

    return error;
}

int
fd_write(void* addr, size_t len, int fd)
{
    int error = 0;

    struct uio auio;
    struct iovec aiov;
    bzero(&aiov, sizeof(struct uio));
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

    return error;
}
