#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sls.h"
#include "sls_wal.h"

/* A single block of written memory in the log. */
struct sls_wal_block {
	void *dest;
	atomic_size_t size;
	unsigned char data[];
};

/* The write-ahead log header. */
struct sls_wal_header {
	atomic_size_t offset;
};

/* Get the size of a block including metadata. */
static size_t
sls_wal_block_size(size_t size)
{
	size_t block_size = sizeof(struct sls_wal_block) + size;
	size_t align_mask = alignof(struct sls_wal_block) - 1;

	return (block_size + align_mask) & ~align_mask;
}

/* Get the maximum possible size of a block. */
static size_t
sls_wal_max_size(const struct sls_wal *wal)
{
	return wal->size - sizeof(struct sls_wal_header);
}

/* Get the header of the log. */
static struct sls_wal_header *
sls_wal_header(struct sls_wal *wal)
{
	return (struct sls_wal_header *)wal->mapping;
}

/* Get the first block in the log, if one exists. */
static struct sls_wal_block *
sls_wal_first(struct sls_wal *wal)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	size_t offset = atomic_load_explicit(&header->offset, memory_order_relaxed);

	if (offset > sizeof(struct sls_wal_header)) {
		return (struct sls_wal_block *)(wal->mapping + sizeof(*header));
	} else {
		return NULL;
	}
}

/* Get the next block in the log, if one exists. */
static struct sls_wal_block *
sls_wal_next(struct sls_wal *wal, struct sls_wal_block *block)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	size_t limit, offset;

	limit = atomic_load_explicit(&header->offset, memory_order_relaxed);

	offset = (char *)block - wal->mapping;
	offset += sls_wal_block_size(block->size);

	if (offset < limit) {
		return (struct sls_wal_block *)(wal->mapping + offset);
	} else {
		return NULL;
	}
}

/* Allocate a new block in the log. */
static struct sls_wal_block *
sls_wal_reserve(struct sls_wal *wal, size_t size)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	size_t block_size = sls_wal_block_size(size);
	size_t offset;

	if (block_size > sls_wal_max_size(wal)) {
		return NULL;
	}

	offset = atomic_fetch_add(&header->offset, block_size);
	if (offset + block_size <= wal->size) {
		return (struct sls_wal_block *)(wal->mapping + offset);
	} else {
		return NULL;
	}
}

/* Get the current SLS epoch number. */
static uint64_t
sls_wal_epoch(struct sls_wal *wal)
{
	uint64_t epoch;

	if (sls_epoch(wal->oid, &epoch) != 0) {
		abort();
	}

	return epoch;
}

/* Wait for a complete snapshot to occur, then clear the log. */
static void
sls_wal_sync(struct sls_wal *wal)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	uint64_t epoch = sls_wal_epoch(wal);
	size_t offset;

	do {
		usleep(10);
	} while (sls_wal_epoch(wal) == epoch);

	pthread_mutex_lock(&wal->mutex);

	offset = atomic_load_explicit(&header->offset, memory_order_relaxed);
	if (offset >= wal->size) {
		memset(wal->mapping + sizeof(*header), 0, wal->size - sizeof(*header));
		atomic_store(&header->offset, sizeof(*header));
	}

	pthread_mutex_unlock(&wal->mutex);
}

int
sls_wal_open(struct sls_wal *wal, uint64_t oid, const char *path, size_t size)
{
	struct sls_wal_header *header;

	wal->fd = open(path, O_RDWR | O_CREAT, 0600);
	if (wal->fd < 0) {
		goto err;
	}

	if (ftruncate(wal->fd, size) != 0) {
		goto err_close;
	}

	wal->mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, wal->fd, 0);
	if (wal->mapping == MAP_FAILED) {
		goto err_close;
	}

	wal->size = size;

	if (pthread_mutex_init(&wal->mutex, NULL) != 0) {
		goto err_munmap;
	}

	header = sls_wal_header(wal);
	atomic_store_explicit(&header->offset, sizeof(*header), memory_order_relaxed);

	memset(wal->mapping + sizeof(*header), 0, size - sizeof(*header));

	return 0;

err_munmap:
	munmap(wal->mapping, wal->size);
err_close:
	close(wal->fd);
err:
	return -1;
}

void
sls_wal_memcpy(struct sls_wal *wal, void *dest, const void *src, size_t size)
{
	struct sls_wal_block *block;

	memcpy(dest, src, size);

	block = sls_wal_reserve(wal, size);
	if (block == NULL) {
		sls_wal_sync(wal);
		return;
	}

	memcpy(block->data, src, size);
	block->dest = dest;
	atomic_store(&block->size, size);
}

void
sls_wal_replay(struct sls_wal *wal)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	struct sls_wal_block *block = sls_wal_first(wal);
	size_t offset = sizeof(*header), size;

	while (block) {
		size = atomic_load_explicit(&block->size, memory_order_relaxed);
		if (size == 0) {
			// A snapshot was taken with an incomplete block, so stop here
			break;
		}

		memcpy(block->dest, block->data, size);
		offset += sls_wal_block_size(size);
		block = sls_wal_next(wal, block);
	}

	// Clear any incomplete blocks
	memset(wal->mapping + offset, 0, wal->size - offset);
	atomic_store_explicit(&header->offset, offset, memory_order_relaxed);
}

int
sls_wal_close(struct sls_wal *wal)
{
	int ret = 0, error = 0;

	if (pthread_mutex_destroy(&wal->mutex) != 0) {
		ret = -1;
		error = errno;
	}

	if (munmap(wal->mapping, wal->size) != 0) {
		ret = -1;
		error = errno;
	}

	if (close(wal->fd) != 0) {
		ret = -1;
		error = errno;
	}

	errno = error;
	return ret;
}
