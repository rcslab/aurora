#include <sys/param.h>
#include <sys/types.h>
#include <sys/buf.h>

#include "slos.h"
#include "slos_inode.h"
#include "slsfs_alloc.h"

#define NEWOSDSIZE (30)
#define AMORTIZED_CHUNK (10000)

/*
 * Generic uint64_t comparison function.
 */
static int
uint64_t_comp(const void *k1, const void *k2)
{
	const uint64_t * key1 = (const uint64_t *)k1;
	const uint64_t * key2 = (const uint64_t *)k2;

	if (*key1 > *key2) {
		return 1;
	} else if (*key1 < *key2) {
		return -1;
	}
	return 0;
}

static int
fast_path(struct slos *slos, uint64_t blocks, diskptr_t *ptr)
{
	uint64_t blksize = BLKSIZE(slos);
	diskptr_t *chunk = &slos->slsfs_alloc.chunk;
	if (chunk->size >=  blocks * blksize) {
		ptr->offset = chunk->offset;
		ptr->size = blocks * blksize;
		chunk->offset += blocks;
		chunk->size -= blocks *blksize;
		return (0);
	}
	return (-1);
}

static int
allocate_chunk(struct slos *slos, diskptr_t *ptr)
{
	struct fnode_iter iter;
	uint64_t temp;
	uint64_t fullsize;
	uint64_t off;
	uint64_t location;

	uint64_t blksize = BLKSIZE(slos);
	uint64_t asked = AMORTIZED_CHUNK;
	int error;

	fbtree_keymax_iter(STREE(slos), &asked, &iter);
	fullsize = ITER_KEY_T(iter, uint64_t);
	off = ITER_VAL_T(iter, uint64_t);
	location = off;

	/* Temporarily remove the extent from the allocator. */
	error = fbtree_remove(STREE(slos), &fullsize, &off);
	if (error == ROOTCHANGE) {
		slos->slos_sb->sb_allocsize = (diskptr_t){ STREE(slos)->bt_root, BLKSIZE(slos) };
	} else if (error) {
		panic("Problem removing element in allocation");
	}

	KASSERT(fullsize >= asked, ("Simple allocation first"));

	error = fbtree_remove(OTREE(slos), &off, &temp);
	if (error == ROOTCHANGE) {
		slos->slos_sb->sb_allocoffset = (diskptr_t){ OTREE(slos)->bt_root, BLKSIZE(slos) };
	} else if (error) {
		panic("Failure in allocation - %d", error);
	}

	/*
	 * Carve off as much as we need, and put the rest back in.
	 * XXX Implement buckets.
	 */
	KASSERT(temp == fullsize, ("Should be reverse mappings"));
	fullsize -= asked;
	off += asked;

	error = fbtree_insert(STREE(slos), &fullsize, &off);
	if (error == ROOTCHANGE) {
		slos->slos_sb->sb_allocsize = (diskptr_t){ STREE(slos)->bt_root, BLKSIZE(slos) };
	} else if (error) {
		panic("Problem removing element in allocation");
	}

	error = fbtree_insert(OTREE(slos), &off, &fullsize);
	if (error == ROOTCHANGE) {
		slos->slos_sb->sb_allocoffset = (diskptr_t){ OTREE(slos)->bt_root, BLKSIZE(slos) };
	} else if (error) {
		panic("Problem removing element in allocation");
	}

	ptr->offset = location;
	ptr->size = asked * blksize;
	return (0);
}

/*
 * Generic block allocator for the SLOS. We never explicitly free.
 */
static int
slsfs_blkalloc(struct slos *slos, size_t bytes, diskptr_t *ptr)
{
	uint64_t asked;
	uint64_t blksize = BLKSIZE(slos);
	diskptr_t *chunk = &slos->slsfs_alloc.chunk;
	int error;

	/* Get an extent large enough to cover the allocation. */
	asked = (bytes + blksize - 1) / blksize;
	BTREE_LOCK(STREE(slos), LK_EXCLUSIVE);
	while (true) {
		if (!fast_path(slos, asked, ptr)) {
			BTREE_UNLOCK(STREE(slos), 0);
			return (0);
		}

		BTREE_LOCK(OTREE(slos), LK_EXCLUSIVE);
		if (chunk->size > bytes) {
			BTREE_UNLOCK(OTREE(slos), 0);
			continue;
		}
		error = allocate_chunk(slos, &slos->slsfs_alloc.chunk);
		if (error) {
			panic("Problem allocating\n");
		}
		
		BTREE_UNLOCK(OTREE(slos), 0);
	}
}

/*
 * Initialize the in-memory allocator state at mount time.
 */
int
slsfs_allocator_init(struct slos *slos)
{
	struct slos_node *offt;
	struct slos_node *sizet;
	diskptr_t ptr;
	uint64_t off;
	uint64_t total;
	uint64_t offbl = slos->slos_sb->sb_allocoffset.offset;
	uint64_t sizebl = slos->slos_sb->sb_allocsize.offset;

	/* Create the in-memory vnodes from the on-disk state. */
	offt = slos_vpimport(slos, offbl);
	sizet = slos_vpimport(slos, sizebl);

	slos->slsfs_alloc.a_offset = offt;
	slos->slsfs_alloc.a_size = sizet;
	slos->slsfs_alloc.chunk.offset = 0;
	slos->slsfs_alloc.chunk.size = 0;

	// We just have to readjust the elements in the btree since we are not
	// using them for the same purpose of keeping track of data
	DBUG("Initing Allocator\n");
	DBUG("%lu %lu\n", offbl, sizebl);
	MPASS(offt && sizet);
	fbtree_init(offt->sn_fdev, offt->sn_tree.bt_root, sizeof(uint64_t), sizeof(uint64_t),
	    &uint64_t_comp, "Off Tree", 0, OTREE(slos));
	fbtree_init(sizet->sn_fdev, sizet->sn_tree.bt_root, sizeof(uint64_t), sizeof(uint64_t),
	    &uint64_t_comp, "Size Tree", 0, STREE(slos));

	// New tree add the initial amount allocations.  Im just making some
	// constant just makes it easier
	/*
	 * If the allocator is uninitialized, populate the trees with the initial values.
	 * TODO Error handling for fbtree_insert().
	 */
	if (!fbtree_size(&offt->sn_tree)) {

		printf("First time start up for allocator\n");
		off = 0;
		total = slos->slos_sb->sb_size;

		fbtree_insert(OTREE(slos), &off, &total);
		fbtree_insert(STREE(slos), &total, &off);

		/*
		 * Carve of a region from the beginning of the device.
		 * We have statically allocated some of these blocks using
		 * the userspace tool, so we retroactively log that allocation
		 * using the call below.
		 * TODO: More dynamic allocation that does exactly the allocations
		 * done?
		 */
		slsfs_blkalloc(slos, NEWOSDSIZE * BLKSIZE(slos), &ptr);
		KASSERT(ptr.offset == 0 , ("should be zero"));
	}

	/* Bind the allocator function to the SLOS. */
	slos->slsfs_blkalloc = &slsfs_blkalloc;

	return (0);
};

/*
 * Initialize the in-memory allocator state at mount time.
 */
int
slsfs_allocator_uninit(struct slos *slos)
{
    slos_vpfree(slos, slos->slsfs_alloc.a_offset);
    slos->slsfs_alloc.a_offset = NULL;
    slos_vpfree(slos, slos->slsfs_alloc.a_size);
    slos->slsfs_alloc.a_size = NULL;

    return (0);
}

/*
 * Flush the allocator state to disk.
 */
int
slsfs_allocator_sync(struct slos *slos)
{
	return (0);
}

