#ifndef _SLSFILE_H_
#define _SLSFILE_H_

#include <sys/param.h>

#include <sys/file.h>
#include <vm/vm_object.h>

#include "sls_mem.h"

int file_read(void* addr, size_t len, int fd);
int file_write(void* addr, size_t len, int fd);
int file_pread(void* addr, size_t len, int fd, size_t off);
int file_pwrite(void* addr, size_t len, int fd, size_t off);

#endif /* _SLSFILE_H */
