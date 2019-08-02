#ifndef _SLSFILE_H_
#define _SLSFILE_H_

#include <sys/param.h>

#include <sys/file.h>
#include <sys/uio.h>

#include <vm/vm_object.h>

#include "sls_mem.h"

int sls_file_read(void* addr, size_t len, struct file *fp);
int sls_file_write(void* addr, size_t len, struct file *fp);
int sls_fd_read(void* addr, size_t len, int fd);
int sls_fd_write(void* addr, size_t len, int fd);
int file_writev(struct iovec *iov, size_t iovlen, int fd);

extern uint64_t size_sent;
#endif /* _SLSFILE_H */
