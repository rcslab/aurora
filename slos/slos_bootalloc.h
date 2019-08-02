#ifndef _SLOS_BOOTALLOC_H_
#define _SLOS_BOOTALLOC_H_

#include <sys/param.h>

struct slos;

/*
 * Dead simple stack allocator that exclusively
 * services the dual btrees of the main allocator.
 * Since only one-block size regions are requested,
 * even a bitmap allocator would be needlessly complex.
 */
struct slos_bootalloc {
	uint64_t	*stack;	    /* Pointer to the data stack */
	uint8_t		*bytemap;   /* Bytemap that tracks allocations for error checking */
	uint64_t	maxsize;    /* The max size of the bootstrap region in blocks */
	uint64_t	size;	    /* The current size of the bootstrap region in blocks */
	uint64_t	succeeded;  /* Total size of successful allocations */
	uint64_t	failed;	    /* Total size of successful allocations */
	uint64_t	frees;	    /* Total size of frees */
};

int slos_bootinit(struct slos *slos);
void slos_bootdestroy(struct slos *slos);
struct slos_diskptr slos_bootalloc(struct slos_bootalloc *alloc);
void slos_bootfree(struct slos_bootalloc *alloc, struct slos_diskptr diskptr);
void slos_bootprint(struct slos_bootalloc *alloc);

#ifdef SLOS_TESTS
int slos_test_bootalloc(void);

#endif /* SLOS_TESTS */

#endif /* _SLOS_BOOTALLOC_H_ */
