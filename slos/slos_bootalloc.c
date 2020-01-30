#include <sys/param.h>

#include <slsfs.h>
#include <slos.h>

#include "slos_btree.h"
#include "slos_bnode.h"
#include "slos_bootalloc.h"
#include "slosmm.h"

/* 
 * Creates a map of used blocks in the btree area. 
 * Used for reconstructing the bootstrap allocator
 * if the disk is dirty and needs to be checked.
 */
static int
slos_bytemap(struct slos *slos, uint64_t rootblk, uint8_t *bytemap, int follow_values)
{
	struct bnode **bnodes;
	struct bnode *bnode, *bparent;
	struct slos_diskptr diskptr;
	uint64_t bootsize;
	uint64_t boffset;
	int error = 0, i;
	
	/* 
	 * We include the superblock in the bytemap because 
	 * it makes calculations easier. We can then ignore
	 * it wherever we actually use the map.
	 */
	bootsize = slos->slos_sb->sb_data.offset;
	bnodes = malloc(sizeof(*bnodes) * bootsize, M_SLOS, M_WAITOK | M_ZERO);

	/* Start from the root of the btree. */
	error = bnode_read(slos, rootblk, &bnode);
	if (error != 0) {
	    DBUG("Error reading rootblk\n");
	    goto error;
	}

	/* 
	 * Mark the block as in-use and point the in-memory bnode there.
	 * We're traversing the whole btree, so we only free the in-memory
	 * bnodes at the end in order to avoid reading them in multiple times.
	 */
	if (bytemap[bnode->blkno] != 0) {
	    DBUG("Block %ld found twice\n", bnode->blkno);
	    error = EINVAL;
	    goto error;
	}
	bytemap[bnode->blkno] = 1;
	bnodes[bnode->blkno] = bnode;
	
	/* Reach the leftmost node. */
	while (bnode->external == BNODE_INTERNAL) {
	    bparent = bnode;
	    bnode = bnode_child(slos, bnode, 0);
	    if (bnode == NULL)
		DBUG("Error bytemap no child\n");
		goto error;

	    /* Lazy update of parent pointers in the path. */
	    if (bnode->parent.offset != bparent->blkno) {
		bnode->parent.offset = bparent->blkno;
		error = bnode_write(slos, bnode);
		if (error != 0)
		    DBUG("Error writing block\n");
		    goto error;
	    }
	    
	    /* If we have already found the bnode something is wrong. */
	    if (bytemap[bnode->blkno] != 0) {
		DBUG("Block %ld found twice\n", bnode->blkno);
		error = EINVAL;
		goto error;
	    }

	    /* Mark the block of the bnode as used, and save it. */
	    bytemap[bnode->blkno] = 1;
	    bnodes[bnode->blkno] = bnode;
	}

	/* Inorder traverse the btree. */
	for (;;) {
	    /* 
	     * If the values are roots of btrees, then we should 
	     * recursively traverse these. We assume that _their_
	     * values aren't pertinent, though, so we don't pass
	     * the flag on.
	     */
	    if (bnode->external == BNODE_EXTERNAL && follow_values != 0) {
		for (i = 0; i < bnode->size; i++) {
		    bnode_getvalue(bnode, i, &diskptr);
		    error = slos_bytemap(slos, diskptr.offset, bytemap, 0);
		    if (error != 0) {
			goto error;
		    }
		}
	    }

	    /* 
	     * If we are at the root, we are done -
	     * free it up and stop the iteration.
	     */
	    if (bnode->parent.offset == bnode->blkno)
		break;

	    /* 
	     * By the way we are traversing the btree,
	     * we already have the parent in memory.
	     */
	    bparent = bnodes[bnode->parent.offset];

	    boffset = bnode_parentoff(bnode, bparent);
	    /* 
	     * If we have a right sibling go to the leftmost node
	     * of the subtree whose root is the sibling.
	     */
	    if (boffset < bparent->size) {

		bnode = bnode_child(slos, bparent, boffset + 1);
		if (bnode == NULL) {
		    goto error;
		}

		/* Lazy update of parent pointers in the path. */
		if (bnode->parent.offset != bparent->blkno) {
		    bnode->parent.offset = bparent->blkno;
		    error = bnode_write(slos, bnode);
		    if (error != 0) {
			goto error;
		    }
		}

		if (bytemap[bnode->blkno] != 0) {
		    DBUG("Block %ld found twice\n", bnode->blkno);
		    error = EINVAL;
		    goto error;
		}
		bytemap[bnode->blkno] = 1;
		bnodes[bnode->blkno] = bnode;

		/* Go down the left side of the subtree. */
		while (bnode->external == BNODE_INTERNAL) {
		    bparent = bnode;

		    bnode = bnode_child(slos, bnode, 0);
		    if (bnode == NULL)
			goto error;

		    /* Lazy update of parent pointers in the path. */
		    if (bnode->parent.offset != bparent->blkno) {
			bnode->parent.offset = bparent->blkno;
			error = bnode_write(slos, bnode);
			if (error != 0)
			    goto error;
		    }

		    if (bytemap[bnode->blkno] != 0) {
			DBUG("Block %ld found twice\n", bnode->blkno);
			error = EINVAL;
			goto error;
		    }
		    bytemap[bnode->blkno] = 1;
		    bnodes[bnode->blkno] = bnode;
		}
	    } else {
		/* If we're the rightmost child go to the parent. */
		bnode = bnodes[bnode->parent.offset];
	    }
	}
	
	/* 
	 * Destroy all the in-memory bnodes (if the space was free
	 * the pointer is null and the free call a no-op).
	 */
	for (i = 0; i < bootsize; i++)
	    free(bnodes[i], M_SLOS);

	free(bnodes, M_SLOS);

	return 0;

error:

	for (i = 0; i < bootsize; i++)
	    free(bnodes[i], M_SLOS);

	free(bnodes, M_SLOS);

	return error;
}


/* 
 * Fill a boot allocator with all the free 
 * blocks from the appropriate disk area.
 */
static int
slos_bootpopulate(struct slos *slos, struct slos_bootalloc *alloc)
{
	uint8_t *bytemap;
	size_t size;
	int error = 0, i;

	/* 
	 * Get a map of nodes in use by the allocator btree. The array
	 * can hold information about blocks up to the data section.
	 */
	bytemap = malloc(sizeof(*bytemap) * slos->slos_sb->sb_data.offset, 
		M_SLOS, M_WAITOK | M_ZERO);

	/* Populate the bytemap with the offsets btree. */
	error = slos_bytemap(slos, slos->slos_sb->sb_broot.offset, bytemap, 0);
	if (error != 0) {
	    DBUG("First bytemap error.\n");
	    goto out;
	}

	DBUG("First bytemap done.\n");

	/* 
	 * Keep populating with the sizes btree, and all size buckets. The
	 * last argument makes the function treat each value of the btree as
	 * a block number to the root of another btree, and traverses that
	 * one as well.
	 */
	error = slos_bytemap(slos, slos->slos_sb->sb_szroot.offset, bytemap, 1);
	if (error != 0) {
	    goto out;
	}

	/* 
	 * We begin from the start of the allocator region because the bytemap
	 * includes the superblock and the allocator root, which are never free.
	 */
	size = slos->slos_sb->sb_bootalloc.offset + slos->slos_sb->sb_bootalloc.size;
	for (i = slos->slos_sb->sb_bootalloc.offset; i < size; i++) {
	    /* 
	     * Push the block into the stack if 
	     * it is not already in the btree.
	     */
	    if (bytemap[i] == 0)
		alloc->stack[alloc->size++] = i;
	    else {
		DBUG("Block %d is in use\n", i);
		alloc->bytemap[i] = 1;
	    }
	}

out:
	free(bytemap, M_SLOS);

	return error;
}
/* 
 * Initialize an in-memory representation of the allocator. 
 * Since the allocator gives out single blocks, the values
 * that it returns are of type uint64_t, not diskptr.
 *
 */
int
slos_bootinit(struct slos *slos)
{
	struct slos_bootalloc *alloc;
	int error;
	

	/* Add room for the bootstrap region. */
	alloc = malloc(sizeof(*alloc), M_SLOS, M_WAITOK | M_ZERO);
	alloc->bytemap = malloc(sizeof(*alloc->bytemap) * slos->slos_sb->sb_data.offset, M_SLOS, M_WAITOK | M_ZERO);
	alloc->maxsize = slos->slos_sb->sb_bootalloc.size;
	alloc->size = 0;
	alloc->stack = malloc(alloc->maxsize * sizeof(*alloc->stack), M_SLOS, M_WAITOK);

	/* Read the btree into memory to find unused space. */
	error = slos_bootpopulate(slos, alloc);
	if (error != 0) {
	    free(alloc->stack, M_SLOS);
	    free(alloc, M_SLOS);

	    return error;
	}

	slos->slos_bootalloc = alloc;

	return 0;
}

/*
 * Destroy the bootstrap allocator and any associated data. 
 */
void
slos_bootdestroy(struct slos *slos)
{
	if (slos->slos_bootalloc == NULL)
	    return;

	free(slos->slos_bootalloc->bytemap, M_SLOS);
	free(slos->slos_bootalloc->stack, M_SLOS);
	free(slos->slos_bootalloc, M_SLOS);
	slos->slos_bootalloc = NULL;
}


/*
 * Return an on-disk block for use by the main allocator.
 */
struct slos_diskptr
slos_bootalloc(struct slos_bootalloc *alloc)
{
	uint64_t blkid;
	struct slos_diskptr ptr;

	/* Check if the bootstrap region is empty. */
	if (alloc->size == 0) {
	    alloc->failed += 1;
	    return DISKPTR_BLOCK(0);
	}

	/* Pop the element from the stack. */
	blkid = alloc->stack[--alloc->size]; 
	ptr = DISKPTR_BLOCK(blkid);

	/* Update statistics. */
	alloc->succeeded += 1;
	if (alloc->bytemap[blkid] != 0)
	    DBUG("ERROR: DOUBLE ALLOC FOR %ld\n", blkid);
	alloc->bytemap[blkid] = 1;

	return ptr;
}


void
slos_bootfree(struct slos_bootalloc *alloc, struct slos_diskptr diskptr)
{
	/* The equivalent of freeing a NULL pointer. */
	if (diskptr.size == 0)
	    return;
	    
	KASSERT(alloc->size <= alloc->maxsize, ("bootalloc_free in bounds"));
	KASSERT(diskptr.size == 1, ("disk pointer is one block wide"));
	KASSERT(alloc->bytemap[diskptr.offset] == 1, ("freeing unallocated block"));

	/* Push the element into the stack. */
	alloc->stack[alloc->size++] = diskptr.offset;

	alloc->bytemap[diskptr.offset] = 0;

	/* Update statistics. */
	alloc->frees += 1;
}

void
slos_bootprint(struct slos_bootalloc *alloc)
{
	DBUG("Bootstrap allocator %p\n", alloc);
	DBUG("Current Size: %lu\tMaximum Size: %lu\n", alloc->size, alloc->maxsize);
	DBUG("Successful Allocs: %lu\tFailed Allocs: %lu\tFrees: %lu\n", 
		alloc->succeeded, alloc->failed, alloc->frees);
}


#ifdef SLOS_TESTS 


#define ITERATIONS  10000	/* Iterations of test */

#define UNALLOCATED 0		/* Status of never allocated elements */
#define ALLOCATED   1		/* Status of currently allocated elements */ 
#define FREED	    2		/* Status of freed elements */

#define PALLOC	    80		/* Weight of alloc operation */
#define PFREE	    20		/* Weight of free operation */

int
slos_test_bootalloc(void)
{
	uint64_t offset, total;
	struct slos_diskptr diskptr;
	uint64_t blkid;
	uint8_t *status;
	size_t size;
	int error = 0, i, iter;
	int operation;

	/* Use an array to hold the status of each block. */
	size = slos.slos_sb->sb_bootalloc.offset + slos.slos_sb->sb_bootalloc.size;
	status = malloc(sizeof(uint64_t) * size, M_SLOS, M_WAITOK);

	/* Destroy any existing allocators. */
	if (slos.slos_bootalloc != NULL) {
	    slos_bootdestroy(&slos);
	}

	/* Initialize the allocator. */
	error = slos_bootinit(&slos);
	if (error != 0) {
	    free(status, M_SLOS);
	    return error;
	}

	/* 
	 * We assume all blocks are allocated, the go into the stack
	 * and find which ones are actually free. That way, we can run
	 * the test even when a root btree is present. 
	 *
	 * Since we don't need to write anything to disk to check the boot
	 * allocator's correctness (it's a transient data structure and is rebuilt
	 * each time at load time), we don't clobber the existing allocator
	 * btree. We can thus combine this test with others if we recreate
	 * the allocator after the test.
	 */
	memset(status, ALLOCATED, size);
	for (i = 0; i < slos.slos_bootalloc->size; i++)
	    status[slos.slos_bootalloc->stack[i]] = UNALLOCATED;


	for (iter = 0; iter < ITERATIONS; iter++) {
	    /* Pick whether to allocate or free at random. */
	    operation = random() % (PALLOC + PFREE);

	    if (operation < PALLOC) {

		/* Do the allocation, keep the block pointer in the btree. */
		diskptr = slos_bootalloc(slos.slos_bootalloc);
		blkid = diskptr.offset;
		/* 
		 * If we failed to allocate completely, check 
		 * if we are actually empty.
		 */
		if (blkid == 0) {
		    if (slos.slos_bootalloc->size != 0) {
			DBUG("ERROR: Allocator has size %lu but failed\n", 
				slos.slos_bootalloc->size);
			error = EINVAL;
			goto out;
		    } else {
			/* If we are actually empty, go to next round. */
			continue;
		    }
		}

		/* 
		 * We don't check if it's allocated, just in case 
		 * we get memory corruption and status is invalid.
		 */
		if (status[blkid] != UNALLOCATED && status[blkid] != FREED) {
		    DBUG("ERROR: Allocated element %lu has status %d\n", 
			    blkid, status[blkid]);
		    error = EINVAL;
		    goto out;
		}

		status[blkid] = ALLOCATED;

	    } else {
		/* Choose a random offset for freeing for allocation. */
		offset = 1 + random() % size;

		/* Find an allocated block starting from the given offset. */
		for (i = offset; i < size; i++) {
		    if (status[i] == ALLOCATED)
			break;
		}

		/* If we didn't find any to the right, start looking to the left. */
		if (i == size) {
		    for (i = offset; i >= 1; i--) {
			if (status[i] == ALLOCATED)
			    break;
		    }
		}

		/* If we didn't find any, continue. */
		if (i == 0)
		    continue;
		
		/* Do bookkeeping and free. */
		status[i] = FREED;

		slos_bootfree(slos.slos_bootalloc, DISKPTR_BLOCK(i));
	    }
	}

out:
	total = 0;
	/* Ignore the superblock, it's always allocated. */
	for (i = 1; i < size; i++) {
	    if (status[i] == ALLOCATED)
		total += 1;
	}

	if (total != slos.slos_bootalloc->maxsize - slos.slos_bootalloc->size) {
	    DBUG("ERROR: Allocator is missing %lu blocks, but only %lu can be found\n",
		    total, slos.slos_bootalloc->maxsize - slos.slos_bootalloc->size);
	    error = EINVAL;
	}

	/* Show statistics. */
	slos_bootprint(slos.slos_bootalloc);

	free(status, M_SLOS);

	/* 
	 * Recreate the allocator in case 
	 * there is other code depending
	 * on its existence.
	 */
	slos_bootdestroy(&slos);
	if (error != 0)
	    return error;

	slos_bootinit(&slos);
	if (error != 0)
	    return error;

	return error;

}

#endif /* SLOS_TESTS */
