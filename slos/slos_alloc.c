#include <sys/param.h>

#include <sys/queue.h>

#include "../include/slos_bnode.h"

#include "slos_alloc.h"
#include "slos_bootalloc.h"
#include "slos_btree.h"
#include "slos_internal.h"
#include "slos_io.h"
#include "slosmm.h"

/*
 * Create a new block allocator.
 */
struct slos_blkalloc *
slos_alloc_init(struct slos *slos)
{
	struct slos_blkalloc *alloc;

	alloc = malloc(sizeof(*alloc), M_SLOS, M_WAITOK | M_ZERO);

	/* Get the dual btrees from the disk. */
	alloc->offsets = btree_init(slos, slos->slos_sb->sb_broot.offset, ALLOCBOOT);
	alloc->sizes = btree_init(slos, slos->slos_sb->sb_szroot.offset, ALLOCBOOT);
	
	/* Make the allocator visible. */
	slos->slos_alloc = alloc;

	return alloc;
}

/*
 * Destroy an allocator and all its contents.
 */
void
slos_alloc_destroy(struct slos *slos)
{
	if (slos->slos_alloc == NULL)
	    return; 

	/* Destroy the in-memory btrees. */
	btree_destroy(slos->slos_alloc->offsets);
	btree_destroy(slos->slos_alloc->sizes);

	/* Free the allocator itself. */
	free(slos->slos_alloc, M_SLOS);
	slos->slos_alloc = NULL;
}

void
slos_alloc_print(struct slos_blkalloc *alloc)
{
	printf("Allocator: %p\n", alloc);
	printf("Successful allocations:%lu\tPartially successful:%lu\t",
		alloc->succeeded, alloc->partial);
	printf("Failed allocations:%lu\tFrees:%lu\n", 
		alloc->failed, alloc->frees);
	printf("Total allocation size:%lu\tTotal free size:%lu\n", 
		alloc->size_alloc, alloc->size_freed);

}

/* 
 * When looking into the allocator offset btree, we have the guarantee
 * that the size btree will have entries which agree with the former.
 * The opposite is not true.
 */

/* 
 * Remove an offset from a bucket. If the bucket 
 * is empty, remove it from the btree.
 */
static int
slos_delbucket(struct slos_blkalloc *alloc, uint64_t offset, uint64_t size)
{
	struct btree *bucket = NULL;
	struct slos_diskptr diskptr;
	int is_empty;
	int error;


	/* Find the bucket. */
	error = btree_search(alloc->sizes, size, &diskptr);
	if (error != 0)
	    goto error;

	bucket = btree_init(&slos, diskptr.offset, ALLOCBOOT);
	if (bucket == NULL) {
	    error = EIO;
	    goto error;
	}

	error = btree_delete(bucket, offset);
	if (error != 0)
	    goto error;


	/* If the bucket is empty, remove it from the tree. */
	error = btree_empty(bucket, &is_empty); 
	if (error != 0)
	    goto error;

	if (is_empty != 0) {
	    error = btree_delete(alloc->sizes, size);
	    if (error != 0)
		goto error;

	    if (alloc->sizes->root != slos.slos_sb->sb_szroot.offset) {
		slos.slos_sb->sb_szroot.offset = alloc->sizes->root;
		error = slos_sbwrite(&slos);
		if (error != 0)
		    goto error;

	    }

	    slos_bootfree(slos.slos_bootalloc, diskptr);

	} else if (bucket->root != diskptr.offset) {
	    /* If we have a new root, update the sizes btree. */
	    error = btree_overwrite(alloc->sizes, size, 
		    &DISKPTR_BLOCK(bucket->root), &diskptr);
	    if (error != 0)
		goto error;
	}

	btree_discardelem(bucket);
	btree_destroy(bucket);
	btree_discardelem(alloc->sizes);

	return 0;

error:
	if (bucket != NULL) {
	    btree_keepelem(bucket);
	    btree_destroy(bucket);
	}

	btree_keepelem(alloc->sizes);

	return error;
}

/* 
 * Add an offset to a bucket. If the
 * bucket does not exist, create it.
 */
static int
slos_addbucket(struct slos_blkalloc *alloc, uint64_t offset, uint64_t size)
{
	struct bnode *broot = NULL;
	struct btree *bucket = NULL;
	struct slos_diskptr diskptr;
	int newbucket;
	int error;


	/* Find the bucket. */
	newbucket = 0;
	error = btree_search(alloc->sizes, size, &diskptr);
	if (error == EINVAL) {
	    /* Allocate space on disk. */
	    diskptr = slos_bootalloc(slos.slos_bootalloc);
	    if (diskptr.offset == 0)
		return ENOSPC;

	    /* Manually create the new root. */
	    broot = bnode_alloc(&slos, diskptr.offset, sizeof(uint64_t), BNODE_EXTERNAL);

	    error = bnode_write(&slos, broot);
	    bnode_free(broot);
	    if (error != 0) 
		goto error;

	    newbucket = 1;
	} else if (error != 0) {
	    printf("ERROR: btree_search failed with error %d\n", error);
	    goto error;
	}

	bucket = btree_init(&slos, diskptr.offset, ALLOCBOOT);
	if (bucket == NULL) {
	    error = EIO;
	    goto error;
	}

	error = btree_insert(bucket, offset, &offset);
	if (error != 0)
	    goto error;

	if (newbucket != 0) {
	    error = btree_insert(alloc->sizes, size, &DISKPTR_BLOCK(bucket->root));
	    if (error != 0)
		goto error;

	    /* If the sizes btree root split, update the superblock. */
	    if (alloc->sizes->root != slos.slos_sb->sb_szroot.offset) {
		slos.slos_sb->sb_szroot.offset = alloc->sizes->root;
		error = slos_sbwrite(&slos);
		if (error != 0)
		    goto error;
	    }

	} else if (bucket->root != diskptr.offset) {
	    /* If we changed root, update the sizes btree. */
	    error = btree_overwrite(alloc->sizes, size, 
		    &DISKPTR_BLOCK(bucket->root), &diskptr);
	    if (error != 0)
		goto error;

	}

	btree_discardelem(bucket);
	btree_destroy(bucket);
	btree_discardelem(alloc->sizes);


	return 0;

error:
	if (bucket != NULL) {
	    btree_keepelem(bucket);
	    btree_destroy(bucket);
	}

	btree_keepelem(alloc->sizes);

	return error;
}

/* Resize an a free entry in the allocator btrees. */
static int
slos_resize(struct slos_blkalloc *alloc, uint64_t offset, uint64_t size, uint64_t newsize)
{
	int error;

	/* 
	 * We can't just overwrite the size of te extent, because then 
	 * the offset btree consistency guarantee would not hold. We
	 * will insert the offset back into the btree at the end.
	 */
	error = btree_delete(alloc->offsets, offset);
	if (error != 0)
	    goto error;

	/* Move the offset to the correct bucket. */
	error = slos_delbucket(alloc, offset, size);
	if (error != 0)
	    goto error;

	error = slos_addbucket(alloc, offset, newsize);
	if (error != 0)
	    goto error;

	/* Insert the extent back into the offsets tree. */
	error = btree_insert(alloc->offsets, offset, &newsize);
	if (error != 0)
	    goto error;

	btree_discardelem(alloc->offsets);

	return 0;

error:

	btree_keepelem(alloc->offsets);
	return error;
}

struct slos_diskptr
slos_alloc(struct slos_blkalloc *alloc, uint64_t origsize)
{
	struct btree *bucket = NULL;
	uint64_t offset = 0xaa, unused = 0xbb;
	uint64_t size, pastsize;
	struct slos_diskptr diskptr, bucketptr;
	int partial;
	int error;

	/* 
	 * Fail if asked for a region of size 0.
	 */
	if (origsize == 0)
	    return DISKPTR_BLOCK(0);

	/* 
	 * Backwards goto is in this case just
	 * a means of having tail recursion.
	 */

retry:

	/* 
	 * The size of the allocated region can be different from
	 * the one requested, so keep a copy of the original for later.
	 */
	size = origsize;

	/* 
	 * Search the tree for buckets of the smallest possible region size
	 * which is equal to or larger than the given size (infimum).
	 */
	error = btree_keymax(alloc->sizes, &size, &bucketptr);
	if (error == EINVAL) {
	    /* 
	     * If that fails, we will return a smaller size than expected.
	     * Search the tree for buckets of the largest possible region size
	     * which is equal to or smaller than the given size (supremum).
	     */
	    error = btree_keymin(alloc->sizes, &size, &bucketptr);
	    /* If that fails, we can't do anything - return. */
	    if (error != 0) {
		if (error == EINVAL)
		    alloc->failed += 1;

		return DISKPTR_BLOCK(0);
	    }

	    partial = 1;
	} else if (error != 0)  {
	    /* We had an actual error, abort. */
	    return DISKPTR_BLOCK(0);
	} else {
	    /* 
	     * We have enough memory to fully
	     * satisfy the allocation.
	     */
	    partial = 0;
	}

	/* Get the bucket for the given size. */
	bucket = btree_init(&slos, bucketptr.offset, ALLOCBOOT);
	if (bucket == NULL)
	    return DISKPTR_BLOCK(0);


	/* Randomly grab an offset. */
	error = btree_first(bucket, &offset, &unused);
	if (error != 0)
	    goto error;

	if (offset != unused)
	    goto error;



	/* 
	 * To avoid inconsistency between the brees, we first 
	 * remove the offset from the btree indexed by offset,
	 * and then remove it from the bucket. In the event
	 * of a crash, we might look into the bucket and
	 * try to use the offset we get, only to find
	 * that it is not in the offset btree. In 
	 * that case, we silently correct the 
	 * bucket and restart the operation.
	 */

	/* 
	 * Check if the offset actually is in the offsets btree,
	 * and that it has the proper size.
	 */
	error = btree_search(alloc->offsets, offset, &pastsize);
	if (error == EINVAL) {

	    error = slos_delbucket(alloc, offset, size);
	    if (error != 0)
		goto error;

	    /* Restart the call. */
	    goto retry;

	} else if (error == 0 && pastsize != size) {

	    /* Move the offset to the correct bucket. */
	    error = slos_delbucket(alloc, offset, size);
	    if (error != 0)
		goto error;

	    error = slos_addbucket(alloc, offset, pastsize);
	    if (error != 0)
		goto error;

	    /* Restart the call. */
	    goto retry;

	} else if (error != 0) {
	    /* Real error, abort. */
	    goto error;
	}

	/* 
	 * If the region is enough for the whole allocation, 
	 * carve out a region of the requested size. 
	 */
	if (origsize < size) {
	    diskptr = DISKPTR(offset + (size - origsize), origsize);

	    error = slos_resize(alloc, offset, size, size - origsize);

	} else {
	    /* 
	     * If the allocated chunk wasn't enough, or was
	     * exactly large enough, then return all of it.
	     */
	    diskptr = DISKPTR(offset, size);

	    error = btree_delete(alloc->offsets, offset);
	    if (error != 0)
		goto error;

	    error = slos_delbucket(alloc, offset, size);

	}

	if (error != 0)
	    goto error;


	/* If the offsets btree changed root, update the superblock. */
	if (alloc->offsets->root != slos.slos_sb->sb_broot.offset) {
	    slos.slos_sb->sb_broot.offset = alloc->offsets->root;
	    error = slos_sbwrite(&slos);
	    if (error != 0)
		goto error;
	}

	/* If everything went well, update statistics. */
	alloc->size_alloc += diskptr.size;

	if (partial != 0)
	    alloc->partial += 1;
	else
	    alloc->succeeded += 1;

	btree_discardelem(bucket);
	btree_destroy(bucket);
	btree_discardelem(alloc->offsets);

	return diskptr;

error:

	if (bucket != NULL) {
	    btree_keepelem(bucket);
	    btree_destroy(bucket);
	}
	btree_keepelem(alloc->offsets);

	return DISKPTR_BLOCK(0);
}

/*
 * The free function of the allocator. Each call gets a block region,
 * and adds it to both btrees of the allocator. If the freed block
 * is contiguous with another, they are coalesced into one. After the
 * call, diskptr is _freed_, since slos_free roughly works as its destructor.
 */
void
slos_free(struct slos_blkalloc *alloc, struct slos_diskptr diskptr)
{
	uint64_t minoffset, maxoffset;
	uint64_t minsize, maxsize;
	uint64_t offset, size;
	int error;

	offset = diskptr.offset;
	size = diskptr.size;

	/* The equivalent of a NULL pointer. */
	if (size == 0)
	    return;

	/* The smallest possible key larger than the offset. */
	minoffset = diskptr.offset;
	error = btree_keymax(alloc->offsets, &minoffset, &minsize);
	if (error != 0 && error != EINVAL)
	    goto error;

	/* The largest possible key smaller than the offset. */
	maxoffset = diskptr.offset;
	error = btree_keymin(alloc->offsets, &maxoffset, &maxsize);
	if (error != 0 && error != EINVAL)
	    goto error;
    
	/* 
	 * We can coalesce. This means resizing 
	 * an entry instead of adding a new one. 
	 */
	if (maxoffset + maxsize == diskptr.offset) {
	    /* Remove the left extent before merging it in. */
	    error = btree_delete(alloc->offsets, maxoffset);
	    if (error != 0)
		goto error;

	    error = slos_delbucket(alloc, maxoffset, maxsize);
	    if (error != 0)
		goto error;

	    /* 
	     * Update the extent to be used 
	     * when trying to coalesce to 
	     * the right.
	     */
	    offset = maxoffset;
	    size = maxsize + size;

	}

	if (offset + size == minoffset) {
	    /* Remove the right extent before merging it in. */
	    error = btree_delete(alloc->offsets, minoffset);
	    if (error != 0)
		goto error;

	    error = slos_delbucket(alloc, minoffset, minsize);
	    if (error != 0)
		goto error;

	    size = size + minsize;
	}

	/* Add the new extent to its size bucket. */
	error = slos_addbucket(alloc, offset, size);
	if (error != 0)
	    goto error;

	/* Add the new extent to the offsets btree. */
	error = btree_insert(alloc->offsets, offset, &size);
	if (error != 0)
	    goto error;
	
	/* If the offsets btree changed root, update the superblock. */
	if (alloc->offsets->root != slos.slos_sb->sb_broot.offset) {
	    slos.slos_sb->sb_broot.offset = alloc->offsets->root;
	    error = slos_sbwrite(&slos);
	    if (error != 0)
		goto error;
	}

	/* Update the allocator statistics. */
	alloc->size_freed += diskptr.size;
	alloc->frees += 1;

	
	btree_discardelem(alloc->offsets);

	return;

error:
	btree_keepelem(alloc->offsets);

	return;
}

#ifdef SLOS_TESTS

/*
 * Allocator testing code.
 */

#define ITERATIONS  10000	/* Iterations of test */
#define MAXALLOC    4		/* Maximum blocksize allocation */
#define INITIAL	    0xaa	/* Value of a never used byte */
#define POISON	    0xcf	/* Value of an allocated byte */
#define FREEPOISON  0xbb	/* Value of a freed byte */

#define PALLOC	    100		/* Weight of alloc operation */
#define PFREE	    80		/* Weight of free operation */

int
slos_test_alloc(void)
{
	uint8_t *memory = NULL;
	struct btree *regions = NULL;
	struct slos_diskptr diskptr = DISKPTR_BLOCK(0);
	uint64_t offset, size, total;
	int error = 0, ret, i, j, cnt;
	struct bnode *bnode;
	uint64_t inuse;
	uint64_t curmax;
	int operation;

	/* 
	 * Use a btree to keep track of allocated regions. 
	 * Because all btrees are now on-disk, we have to 
	 * create a new btree in the allocator region 
	 * of the disk if we want to use the same code.
	 * As long as the boot allocator works properly,
	 * this is fine, and the btree is destroyed after
	 * each unmount.
	 */
	diskptr = slos_bootalloc(slos.slos_bootalloc);
	if (diskptr.offset == 0) {
	    error = ENOSPC;
	    goto out;
	}

	bnode = bnode_alloc(&slos, diskptr.offset, 
		sizeof(struct slos_diskptr), BNODE_EXTERNAL);
	error = bnode_write(&slos, bnode);
	if (error != 0)
	    goto out;

	/* Free the bnode, we'll reread it from disk. */
	bnode_free(bnode);

	regions = btree_init(&slos, diskptr.offset, ALLOCBOOT);
	if (regions == NULL) {
	    error = EIO;
	    goto out;
	}

	/* The total size allocated. */
	total = 0;

	/* 
	 * Unfortunately, there is no easy way to know
	 * which data blocks are currently in use at this
	 * level of abstraction. We thus create a bytemap
	 * of the data part of the disk, grab each free 
	 * extent from the allocator, and mark its blocks
	 * as free in the bytemap. We then linearly traverse
	 * it, finding out the occupied extents, and inserting
	 * them into the regions btree.
	 */

	/* Allocate the memory region which we allocate and free. */
	memory = malloc(sizeof(*memory) * slos.slos_sb->sb_size, 
		M_SLOS, M_WAITOK | M_ZERO);

	/* 
	 * Initialize all of it as used, we'll find the free blocks below. 
	 * It also helps that this way we're making the superblock and
	 * boot allocator areas inaccessible.
	 */
	memset(memory, POISON, slos.slos_sb->sb_size * sizeof(*memory));

	/* Grab all existing data extents from the disk .*/
	cnt = 0;
	curmax = slos.slos_sb->sb_data.offset;
	while ((error = btree_keymax(slos.slos_alloc->offsets, &curmax, &size)) == 0) {

	    /* Mark the blocks as free. */
	    for (i = curmax; i < curmax + size; i++)
		memory[i] = INITIAL;

	    curmax = i;
	    cnt += 1;
	}


	if (error != EINVAL)
	    goto out;
	error = 0;


	inuse = 0;
	/* Insert the extents into the btree. */
	for (i = slos.slos_sb->sb_data.offset; i < slos.slos_sb->sb_size;) {
	    if (memory[i] == POISON) {
		/* 
		 * If we found a 0, the block is free. 
		 * Starting from that block, create the
		 * largest extent possible.
		 */
		offset = i;
		while (i < slos.slos_sb->sb_size && memory[i] == POISON)
		    i += 1;
		size = i - offset;
		inuse += size;

		/* Create an in-memory block pointer. */
		diskptr = DISKPTR(offset, size);

		/* Insert the extent into the btree. */
		error = btree_insert(regions, 
			diskptr.offset, &diskptr);
		if (error != 0)
		    goto out;

	    } else {
		/* 
		 * Otherwise it's a free block, increment
		 * until we find a used block. 
		 */
		while (i < slos.slos_sb->sb_size && memory[i] == INITIAL)
		    i += 1;
	    }
	}


	for (i = 0; i < ITERATIONS; i++) {
	    /* Pick whether to allocate or free at random using . */
	    operation = random() % (PALLOC + PFREE);

	    if (operation < PALLOC) {
		/* Choose a random size for allocation. */
		size = random() % MAXALLOC;

		/* Do the allocation, keep the block pointer in the btree. */
		diskptr = slos_alloc(slos.slos_alloc, size);
		/* If we failed to allocate completely, go to the next round. */
		if (diskptr.offset == 0) 
		    continue;


		ret = btree_insert(regions, diskptr.offset, &diskptr);
		if (ret != 0)
		    printf("ERROR: Offset %lu (size %lu) allocated twice\n", 
			    diskptr.offset, diskptr.size);

		/* 
		 * The size returned can be less than what was requested,
		 * but never more.
		 */
		if (diskptr.size > size) {
		    printf("ERROR: (%ld, %ld) returned for request of size %lu\n",
			    diskptr.offset, diskptr.size, size);
		    error = EINVAL;
		    goto out;
		}

		/* 
		 * Traverse the region making sure the allocated block does not
		 * hold the poison (or any other) value. Then set it to the poison
		 * value.
		 */
		for (j = 0; j < diskptr.size; j++) {
		    /* 
		     * If we find the poison value, we allocated the same
		     * region twice. If we find any other value, we have
		     * a more generic memory corruption situation.
		     */
		    if (memory[diskptr.offset + j] != INITIAL && 
			memory[diskptr.offset + j] != FREEPOISON) {
			printf("ERROR: Returned region has invalid data %x in it\n",
				memory[diskptr.offset + j]);
			printf("Poison value: %x\n", POISON);
			error = EINVAL;
			goto out;
		    }

		    memory[diskptr.offset + j] = POISON;

		}

		total += diskptr.size;

	    } else {
		/* Choose a random offset for freeing for allocation. */
		offset = slos.slos_sb->sb_data.offset + (random() % slos.slos_sb->sb_size);

		/* We get the closest offset to that value that we can find. */
		ret = btree_keymin(regions, &offset, &diskptr);
		if (ret == EINVAL) {
		    /* 
		     * If we have no lower bound, look for an upper one.
		     * If we still get no available offset, we just quit
		     * trying and go to the next round.
		     */
		    ret = btree_keymax(regions, &offset, &diskptr);	
		    if (ret == EINVAL) {
			continue;
		    } else if (ret != 0) {
			error = ret; 
			goto out;
		    }
		} else if (ret != 0) {
		    error = ret;
		    goto out;
		}


		/* Free the block pointer. */
		for (j = 0; j < diskptr.size; j++) {
		    /*
		     * Check the values in the region to be freed.
		     * If we see a 0 value, it's possibly a double free.
		     * If it is something else, we have more generic
		     * memory corruption.
		     */
		    if (memory[diskptr.offset + j] != POISON) {
			printf("ERROR: Value %x found in byte instead of poison %x\n",
				memory[diskptr.offset + j], POISON);
			printf("ERROR: Offset of extent: %ld, offset inside: %d\n",
				diskptr.offset, j);
			error = EINVAL;
			goto out;
		    }

		    /* Unset the value in the region. */
		    memory[diskptr.offset + j] = FREEPOISON;

		}

		/* Account for the freed region. */
		total -= diskptr.size;

		/* Remove the offset from the tree of allocated regions. */
		error = btree_delete(regions, diskptr.offset);
		if (error != 0)
		    printf("ERROR: Unallocated offset %lu freed\n", diskptr.offset);

		slos_free(slos.slos_alloc, diskptr);
	    }

	    btree_discardelem(regions);
	}

	printf("Blocks in use before test: %ld\n", inuse);
	slos_alloc_print(slos.slos_alloc);
out:

	if (regions != NULL) {
	    /* Free all diskptr allocated during execution. */
	    ret = btree_first(regions, &offset, &diskptr);
	    while (ret == 0) {

		/* Delete the entry from the tree. */
		error = btree_delete(regions, offset);
		if (error != 0) {
		    printf("ERROR %d: Offset %lu not present anymore\n", error, offset);
		    break;
		}

		/* Check if there are still elements in the tree. */
		ret = btree_first(regions, &offset, &diskptr);
	    }

	    btree_discardelem(regions);
	    btree_destroy(regions);
	}

	/* Free the root of the region btree. */
	/* XXX This is a double free, find out where the hell the other free is. */
	/*
	if (blkno != 0)
	    slos_bootfree(slos.slos_bootalloc, blkno);
	*/

	free(memory, M_SLOS);

	return error;

}

#endif /* SLOS_TESTS */
