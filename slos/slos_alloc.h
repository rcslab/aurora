#ifndef _SLOS_ALLOC_H_
#define _SLOS_ALLOC_H_

#include <sys/param.h>

#include <sys/queue.h>

#include "slos_btree.h"

/* 
 * The allocator itself needs memory when its component btrees grow in 
 * size. Since we can't service the allocator using itself, we have a simple
 * allocator which only allocates blocks of size one from a specified region.
 * Since we only need single blocks for the btree, the allocator can be
 * extremely simple. This allocator is not part of the block allocator,
 * however, but of the struct slos. When allocating for a btree, we can
 * check if it's an allocator btree, and if so we use the bootstrap allocator.
 */

/* The actual allocator datatype. */
struct slos_blkalloc {
	struct btree	*offsets;   /* A btree that maps region offsets to sizes */
	struct btree	*sizes;	    /* A btree that maps sizes to region offsets */
	uint64_t	succeeded;  /* Number of allocations succeeded */
	uint64_t	partial;    /* Number of partially successful allocations */
	uint64_t	failed;	    /* Number of allocations failed */
	uint64_t	frees;	    /* Number of frees */
	uint64_t	size_alloc; /* Total size allocated in blocks */
	uint64_t	size_freed; /* Total size freed in blocks */
};

struct slos_blkalloc *slos_alloc_init(struct slos *slos);
void slos_alloc_destroy(struct slos *slos);
void slos_alloc_print(struct slos_blkalloc *alloc);

struct slos_diskptr slos_alloc(struct slos_blkalloc *alloc, uint64_t origsize);
void slos_free(struct slos_blkalloc *alloc, struct slos_diskptr diskptr);

#ifdef SLOS_TESTS

int slos_test_alloc(void);

#endif /* SLOS_TESTS */

#endif /* _SLOS_ALLOC_H_ */
