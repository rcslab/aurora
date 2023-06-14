#ifndef _SLS_WAL_H_
#define _SLS_WAL_H_

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A write-ahead log for SLS memory transactions. */
struct sls_wal {
	uint64_t oid;
	char *mapping;
	size_t size;
	uint64_t epoch;
	pthread_mutex_t mutex;
};

/* Open a write-ahead log backed by the given file. */
int sls_wal_open(struct sls_wal *wal, uint64_t oid, size_t size);

/* Perform a transactional memcpy(). */
void sls_wal_memcpy(
    struct sls_wal *wal, void *dest, const void *src, size_t size);

/* Make sure the write-ahead log is persisted. */
int sls_wal_sync(struct sls_wal *wal);

/* Replay the operations from a write-ahead log. */
void sls_wal_replay(struct sls_wal *wal);

/* Close a write-ahead log. */
int sls_wal_close(struct sls_wal *wal);

/* Specify a point at which execution will resume after a crash. */
int sls_wal_savepoint(struct sls_wal *wal);

int slsfs_sas_create(char *path, size_t size);
int slsfs_sas_map(int fd, void **addrp);

#ifdef __cplusplus
}
#endif

#endif // _SLS_WAL_H_
