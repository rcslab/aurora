#ifndef _FILEIO_H_
#define _FILEIO_H_

#include <sys/types.h>

enum descriptor_type {
    DESC_FD,
    DESC_MD,
    DESCRIPTOR_SIZE,
};

struct sls_desc{
    enum descriptor_type type; 
    int index;
};

struct sls_desc create_desc(int fd, int fd_type, int restoring);

int fd_read(void* addr, size_t len, struct sls_desc desc);
int fd_write(void* addr, size_t len, struct sls_desc desc);

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
