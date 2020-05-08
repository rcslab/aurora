#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* A write-ahead log for SLS memory transactions. */
struct sls_wal {
	uint64_t oid;
	int fd;
	char *mapping;
	size_t size;
	pthread_mutex_t mutex;
};

/* Open a write-ahead log backed by the given file. */
int sls_wal_open(struct sls_wal *wal, uint64_t oid, const char *path, size_t size);

/* Perform a transactional memcpy(). */
void sls_wal_memcpy(struct sls_wal *wal, void *dest, const void *src, size_t size);

/* Replay the operations from a write-ahead log. */
void sls_wal_replay(struct sls_wal *wal);

/* Close a write-ahead log. */
int sls_wal_close(struct sls_wal *wal);
