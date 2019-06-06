#ifndef _SLSFILE_H_
#define _SLSFILE_H_

#include <sys/param.h>

#include <sys/file.h>
#include <sys/uio.h>

#include <vm/vm_object.h>

#include "sls_mem.h"
#include "sls_mosd.h"

int sls_file_read(void* addr, size_t len, struct file *fp);
int sls_file_write(void* addr, size_t len, struct file *fp);
int sls_fd_read(void* addr, size_t len, int fd);
int sls_fd_write(void* addr, size_t len, int fd);
int file_writev(struct iovec *iov, size_t iovlen, int fd);
int osd_pread(struct osd_mbmp *mbmp, uint64_t block, void *addr, size_t len);
int osd_preadv(struct osd_mbmp *mbmp, uint64_t block, struct iovec *iov, size_t iovlen);
int osd_pwrite(struct osd_mbmp *mbmp, uint64_t block, void *addr, size_t len);
int osd_pwritev(struct osd_mbmp *mbmp, uint64_t block, struct iovec *iov, size_t iovlen);

extern uint64_t size_sent;
#endif /* _SLSFILE_H */
