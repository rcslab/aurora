#ifndef _INMEM_H_
#define _INMEM_H_

#include <sys/types.h>

#define MEM_BLOCK_SIZE (4096 * 16)
#define MEM_BUFF_INIT_SIZE (MEM_BLOCK_SIZE * 4096)

uint8_t *mem_buff;
size_t mem_buff_size;
size_t mem_buff_offset;

struct memory_descriptor *mds;
size_t md_size;
size_t md_offset;

int new_md(void);
int md_init(void);
int md_clear(void);
int md_reset(int md);

int mem_read(void* dst, size_t len, int md);
int mem_write(void* src, size_t len, int md);

struct memory_descriptor {
	size_t *block;
	size_t block_size;
	size_t block_inited;

	size_t block_offset;
	size_t inblock_offset;
};

#endif /* _INMEM_H_ */
