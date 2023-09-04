#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "slos.h"
#include "sls.h"
#include "sls_wal.h"
#include "slsfs.h"

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

	return ((block_size + align_mask) & ~align_mask);
}

/* Get the maximum possible size of a block. */
static size_t
sls_wal_max_size(const struct sls_wal *wal)
{
	return (wal->size - sizeof(struct sls_wal_header));
}

/* Get the header of the log. */
static struct sls_wal_header *
sls_wal_header(struct sls_wal *wal)
{
	return ((struct sls_wal_header *)wal->mapping);
}

/* Get the first block in the log, if one exists. */
static struct sls_wal_block *
sls_wal_first(struct sls_wal *wal)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	size_t offset = atomic_load_explicit(
	    &header->offset, memory_order_relaxed);

	if (offset > sizeof(struct sls_wal_header))
		return (
		    (struct sls_wal_block *)(wal->mapping + sizeof(*header)));
	else
		return (NULL);
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

	if (offset < limit)
		return ((struct sls_wal_block *)(wal->mapping + offset));
	else
		return (NULL);
}

/* Allocate a new block in the log. */
static struct sls_wal_block *
sls_wal_reserve(struct sls_wal *wal, size_t size)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	size_t block_size = sls_wal_block_size(size);
	size_t offset;

	if (block_size > sls_wal_max_size(wal))
		return (NULL);

	offset = atomic_fetch_add(&header->offset, block_size);
	if (offset + block_size <= wal->size)
		return ((struct sls_wal_block *)(wal->mapping + offset));
	else
		return (NULL);
}

/* Wait for a complete snapshot to occur, then clear the log. */
static void
sls_wal_full_checkpoint(struct sls_wal *wal)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	size_t offset;
	int error;

	error = sls_untilepoch(wal->oid, wal->epoch);
	if (error != 0)
		abort();

	pthread_mutex_lock(&wal->mutex);

	offset = atomic_load_explicit(&header->offset, memory_order_relaxed);
	if (offset >= wal->size) {
		memset(wal->mapping + sizeof(*header), 0,
		    wal->size - sizeof(*header));
		atomic_store(&header->offset, sizeof(*header));
	}

	pthread_mutex_unlock(&wal->mutex);
}

int
sls_wal_open(struct sls_wal *wal, uint64_t oid, size_t size)
{
	struct sls_wal_header *header;
	size_t total_size = size + 2 * PAGE_SIZE;
	int error;

	error = sls_attach(SLS_DEFAULT_PARTITION, getpid());
	if (error != 0)
		goto err;

	// Allocate a guard page on either side to prevent coalescing
	char *mapping = mmap(NULL, total_size, PROT_NONE, MAP_GUARD, -1, 0);
	if (mapping == MAP_FAILED)
		goto err;

	wal->mapping = mmap(mapping + PAGE_SIZE, size, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (wal->mapping == MAP_FAILED)
		goto err_munmap;

	wal->size = size;
	wal->oid = oid;

	if (pthread_mutex_init(&wal->mutex, NULL) != 0)
		goto err_munmap;

	header = sls_wal_header(wal);
	atomic_store_explicit(
	    &header->offset, sizeof(*header), memory_order_relaxed);

	memset(wal->mapping + sizeof(*header), 0, size - sizeof(*header));

	error = sls_checkpoint_epoch(oid, true, &wal->epoch);
	if (error != 0)
		goto err_checkpoint;

	return (0);

err_checkpoint:
	pthread_mutex_destroy(&wal->mutex);

	/* Unmap the guard pages created by overlaying the WAL. */
	munmap(&wal->mapping, PAGE_SIZE);
	munmap(&wal->mapping + PAGE_SIZE + size, PAGE_SIZE);

err_munmap:
	munmap(mapping, total_size);
err:
	return (-1);
}

int
sls_wal_savepoint(struct sls_wal *wal)
{
	return (sls_checkpoint_epoch(wal->oid, true, &wal->epoch));
}

void
sls_wal_memcpy(struct sls_wal *wal, void *dest, const void *src, size_t size)
{
	struct sls_wal_block *block;

	memcpy(dest, src, size);

	block = sls_wal_reserve(wal, size);
	if (block == NULL) {
		sls_wal_full_checkpoint(wal);
		return;
	}

	memcpy(block->data, src, size);
	block->dest = dest;
	atomic_store(&block->size, size);
}

int
sls_wal_sync(struct sls_wal *wal)
{
	return (sls_memsnap_epoch(wal->oid, wal->mapping, &wal->epoch));
}

void
sls_wal_replay(struct sls_wal *wal)
{
	struct sls_wal_header *header = sls_wal_header(wal);
	struct sls_wal_block *block = sls_wal_first(wal);
	size_t offset = sizeof(*header), size;

	while (block) {
		size = atomic_load_explicit(&block->size, memory_order_relaxed);
		if (size == 0)
			// A snapshot was taken with an incomplete block, so
			// stop here
			break;

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
	size_t total_size = wal->size + 2 * PAGE_SIZE;

	if (pthread_mutex_destroy(&wal->mutex) != 0) {
		ret = -1;
		error = errno;
	}

	if (munmap(wal->mapping - PAGE_SIZE, total_size) != 0) {
		ret = -1;
		error = errno;
	}

	errno = error;
	return (ret);
}

int
slsfs_create_wal(char *path, int flags, int mode, size_t size)
{
	char *dir;
	int pfd;
	int walfd;

	char dupstr[PATH_MAX];
	struct slsfs_create_wal_args args;

	memset(args.path, '\0', PATH_MAX);
	strncpy(args.path, path, strlen(path));
	args.size = size;
	args.mode = mode;
	args.flags = flags | O_CREAT;

	memset(dupstr, '\0', PATH_MAX);
	strncpy(dupstr, path, strlen(path));

	dir = dirname(dupstr);
	if (dir == NULL) {
		return (EINVAL);
	}

	pfd = open(dir, O_RDONLY);
	walfd = ioctl(pfd, SLSFS_CREATE_WAL, &args);
	close(pfd);

	return (walfd);
}

int
slsfs_sas_create(char *path, size_t size)
{
	struct slsfs_sas_create_args args;
	char dupstr[PATH_MAX];
	int pfd, walfd;
	char *dir;

	memset(args.path, '\0', PATH_MAX);
	strncpy(args.path, path, strlen(path));
	args.size = size;

	memset(dupstr, '\0', PATH_MAX);
	strncpy(dupstr, path, strlen(path));

	dir = dirname(dupstr);
	if (dir == NULL) {
		return (EINVAL);
	}

	pfd = open(dir, O_RDONLY);
	walfd = ioctl(pfd, SLSFS_SAS_CREATE, &args);
	close(pfd);

	return (walfd);
}

int
slsfs_sas_map(int fd, void **addrp)
{
	struct slsfs_sas_create_args args;
	void *addr;
	int error;

	error = ioctl(fd, SLSFS_SAS_MAP, &addr);
	if (error != 0) {
		perror("sas_map");
		return (1);
	}

	*addrp = addr;

	return (0);
}

int
sas_trace_start(int fd)
{
	int error;

	error = ioctl(fd, SLSFS_SAS_TRACE_START);
	if (error != 0) {
		perror("sas_trace_start");
		return (1);
	}

	return (0);
}

int
sas_trace_end(int fd)
{
	int error;

	error = ioctl(fd, SLSFS_SAS_TRACE_END);
	if (error != 0) {
		perror("sas_trace_end");
		return (1);
	}

	return (0);
}

int
sas_trace_commit(int fd)
{
	int error;

	error = ioctl(fd, SLSFS_SAS_TRACE_COMMIT);
	if (error != 0) {
		perror("sas_trace_commit");
		return (1);
	}

	return (0);
}

int
sas_trace_abort(int fd)
{
	int error;

	error = ioctl(fd, SLSFS_SAS_TRACE_ABORT);
	if (error != 0) {
		perror("sas_trace_abort");
		return (1);
	}

	return (0);
}

int
sas_refresh_protection(int fd)
{
	int error;

	error = ioctl(fd, SLSFS_SAS_REFRESH_PROTECTION);
	if (error != 0) {
		perror("sas_trace_abort");
		return (1);
	}

	return (0);
}
