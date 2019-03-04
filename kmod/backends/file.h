#ifndef _SLSFILE_H_
#define _SLSFILE_H_


int file_read(void* addr, size_t len, int fd);
int file_write(void* addr, size_t len, int fd);

#endif /* _SLSFILE_H */
