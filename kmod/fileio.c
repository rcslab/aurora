#include "fileio.h"
#include "_slsmm.h"
#include "slsmm.h"

#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/uio.h>

int 
fd_read(void* addr, size_t len, int d, int type)
{
	if (type == SLSMM_FD_FILE)
		return file_read(addr, len, d);
	if (type == SLSMM_FD_MEM)
		return mem_read(addr, len, d);
	return -1;
}

int 
fd_write(void* addr, size_t len, int d, int type)
{
	if (type == SLSMM_FD_FILE)
		return file_write(addr, len, d);
	if (type == SLSMM_FD_MEM)
		return mem_write(addr, len, d);
	return -1;
}

int
file_read(void* addr, size_t len, int fd)
{
	int error = 0;

	struct uio auio;
	struct iovec aiov;
	bzero(&auio, sizeof(struct uio));
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
	if (error) {
		printf("Error: kern_readv failed with code %d\n", error);
	}

	return error;
}

int
file_write(void* addr, size_t len, int fd)
{
	int error = 0;

	struct uio auio;
	struct iovec aiov;
	bzero(&auio, sizeof(struct uio));
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
	if (error) {
		printf("Error: kern_writev failed with code %d\n", error);
	}

	return error;
}

int
write_buf(uint8_t *buf, void *src, size_t *offset, size_t size, size_t cap) {
	if (*offset + size > cap) {
		printf("Exceed buffer size\n");
		return -1;
	}
	memcpy(buf+*offset, src, size);
	*offset += size;
	return 0;
}

uint8_t *mem_buff;
size_t mem_buff_size;
size_t mem_buff_offset;

struct memory_descriptor *mds;
size_t md_size;
size_t md_offset;


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

int
mem_read(void* dst, size_t len, int md) 
{
	return mem_buff_io(dst, len, md, 0);
}

int 
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
