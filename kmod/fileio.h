#ifndef _FILEIO_H_
#define _FILEIO_H_

#include <sys/types.h>

int fd_read(void* addr, size_t len, int fd);
int fd_write(void* addr, size_t len, int fd);

#endif
