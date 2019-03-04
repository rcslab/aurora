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

#include "fileio.h"
#include "../memckpt.h"
#include "../_slsmm.h"
#include "../slsmm.h"


static int
double_array_size(void** array, size_t cur_size)
{
	printf("double size\n");
	void* new_array;

	new_array = malloc(cur_size * 2, M_SLSMM, M_NOWAIT);
	if (new_array == NULL)
		return ENOMEM;

	memcpy(new_array, *array, cur_size);
	free(*array, M_SLSMM);
	*array = new_array;
	return 0;
}

int
md_init()
{
	mem_buff = malloc(MEM_BUFF_INIT_SIZE, M_SLSMM, M_NOWAIT);
	if (mem_buff == NULL)
		return ENOMEM;
	mem_buff_size = MEM_BUFF_INIT_SIZE;
	mem_buff_offset = 0;

	mds = malloc(sizeof(struct memory_descriptor)*32, M_SLSMM, M_NOWAIT);
	if (mem_buff == NULL)
		return ENOMEM;
	md_size = 32;
	md_offset = 0;
	return 0;
}

int
md_clear()
{
	for (size_t i = 0; i < md_offset; i ++)
		free(mds[i].block, M_SLSMM);
	free(mem_buff, M_SLSMM);
	free(mds, M_SLSMM);
	return 0;
}

static int
new_block(int md)
{
	int error = 0;

	if (mds[md].block_inited + 1 > mds[md].block_size) {
		// double block list size
		error = double_array_size((void*)&(mds[md].block),
				mds[md].block_size * sizeof(uint8_t *));
		if (error)
			return error;
		mds[md].block_size *= 2;
	}

	if (mem_buff_offset + MEM_BLOCK_SIZE > mem_buff_size) {
		// double buffer size
		error = double_array_size((void*)&mem_buff, mem_buff_size);
		if (error)
			return error;
		mem_buff_size *= 2;
	}

	mds[md].block[mds[md].block_inited] = mem_buff_offset;
	mds[md].block_inited ++;
	mem_buff_offset += MEM_BLOCK_SIZE;

	return error;
}

static int
mem_buff_io(void* arr, size_t len, int md, int mode)
{
	int error = 0;
	size_t arr_offset = 0;
	size_t size;
	size_t block_remaining;
	size_t data_remaining;
	size_t md_offset;

	while (arr_offset < len) {
		data_remaining = len - arr_offset;
		block_remaining = MEM_BLOCK_SIZE - mds[md].inblock_offset;

		if (block_remaining == 0) {
			// move to next block
			mds[md].block_offset += 1;
			mds[md].inblock_offset = 0;

			block_remaining = MEM_BLOCK_SIZE;
		}

		if (mds[md].block_offset >= mds[md].block_inited) {
			// assign a block to uninitialized block
			error = new_block(md);
			if (error)
				return error;
		}
		
		size = data_remaining < block_remaining ? data_remaining : block_remaining;
		md_offset = mds[md].block[mds[md].block_offset] + mds[md].inblock_offset;
		if (mode) // write
			memcpy(mem_buff + md_offset, (uint8_t *)arr + arr_offset, size);
		else // read
			memcpy((uint8_t *)arr + arr_offset, mem_buff + md_offset, size);

		arr_offset += size;
		mds[md].inblock_offset += size;
	}

	return 0;
}

int
new_md()
{
	int error = 0;

	if (md_offset + 1 > md_size) {
		error = double_array_size((void*)&mds, md_size * sizeof(struct memory_descriptor));
		if (error)
			return error;
		md_size *= 2;
	}

	mds[md_offset].block = malloc(sizeof(size_t)*32, M_SLSMM, M_NOWAIT);
	mds[md_offset].block_size = 32;
	mds[md_offset].block_inited = 0;
	mds[md_offset].block_offset = 0;
	mds[md_offset].inblock_offset = 0;

	return md_offset ++;
}

static int
mem_read(void* dst, size_t len, int md)
{
	return mem_buff_io(dst, len, md, 0);
}

static int
mem_write(void* src, size_t len, int md)
{
	return mem_buff_io(src, len, md, 1);
}

int
md_reset(int md)
{
	mds[md].block_offset = 0;
	mds[md].inblock_offset = 0;
	return 0;
}

