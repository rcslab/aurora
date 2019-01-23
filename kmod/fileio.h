#ifndef _FILEIO_H_
#define _FILEIO_H_

#include <sys/types.h>

int fd_read(void* addr, size_t len, int fd, int type);
int fd_write(void* addr, size_t len, int fd, int type);

int file_read(void* addr, size_t len, int fd);
int file_write(void* addr, size_t len, int fd);
int write_buf(uint8_t *buf, void *src, size_t *offset, size_t size, size_t cap);

#define MEM_BLOCK_SIZE (4096 * 16)
#define MEM_BUFF_INIT_SIZE (MEM_BLOCK_SIZE * 4096)

struct memory_descriptor {
	size_t *block;
	size_t block_size;
	size_t block_inited;

	size_t block_offset;
	size_t inblock_offset;
};

int new_md(void);
int md_init(void);
int md_clear(void);
int mem_read(void* addr, size_t len, int md);
int mem_write(void* addr, size_t len, int md);
int md_reset(int md);

#endif
