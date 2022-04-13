#include <sys/types.h>
#include <sys/param.h>
#include <sys/buf.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <slos.h>
#include <slos_alloc.h>
#include <slos_btree.h>
#include <slos_inode.h>
#include <slsfs.h>

#include "debug.h"
#include "slos_subr.h"
#include "slsfs_buf.h"

#define NEWOSDSIZE (30)
#define AMORTIZED_CHUNK (1024)

/*
 * Generic uint64_t comparison function.
 */
int
uint64_t_comp(const void *k1, const void *k2)
{
	const uint64_t *key1 = (const uint64_t *)k1;
	const uint64_t *key2 = (const uint64_t *)k2;

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
	diskptr_t *chunk = &slos->slos_alloc.chunk;

	if (chunk->size >= blocks * blksize) {
		ptr->offset = chunk->offset;
		ptr->size = blocks * blksize;
		ptr->epoch = slos->slos_sb->sb_epoch;
		chunk->offset += blocks;
		chunk->size -= blocks * blksize;
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

	error = fbtree_keymax_iter(STREE(slos), &asked, &iter);
	if (error != 0) {
		panic("Problem with keymax %d\n", error);
	}
	if (ITER_ISNULL(iter)) {
		printf("SLOS is full!\n");
		return (ENOSPC);
	}
	fullsize = ITER_KEY_T(iter, uint64_t);
	off = ITER_VAL_T(iter, uint64_t);
	location = off;

	/* Temporarily remove the extent from the allocator. */
	error = fbtree_remove(STREE(slos), &fullsize, &off);
	if (error) {
		panic("Problem removing element in allocation");
	}

	KASSERT(fullsize >= asked, ("Simple allocation first"));

	error = fbtree_remove(OTREE(slos), &off, &temp);
	if (error) {
		panic("Failure in allocation - %d", error);
	}

	/*
	 * Carve off as much as we need, and put the rest back in.
	 * XXX Implement buckets.
	 */
	if (fullsize > asked) {
		KASSERT(temp == fullsize, ("Should be reverse mappings"));
		fullsize -= asked;
		off += asked;

		error = fbtree_insert(STREE(slos), &fullsize, &off);
		if (error) {
			panic("Problem removing element in allocation");
		}

		error = fbtree_insert(OTREE(slos), &off, &fullsize);
		if (error) {
			panic("Problem removing element in allocation");
		}
	}

	ptr->offset = location;
	ptr->size = asked * blksize;
	ptr->epoch = slos->slos_sb->sb_epoch;

	return (0);
}

/*
 * Generic block allocator for the SLOS. We never explicitly free.
 */
int
slos_blkalloc(struct slos *slos, size_t bytes, diskptr_t *ptr)
{
	uint64_t asked;
	uint64_t blksize = BLKSIZE(slos);
	diskptr_t *chunk = &slos->slos_alloc.chunk;
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

		error = allocate_chunk(slos, &slos->slos_alloc.chunk);
		if (error != 0) {
			panic("Problem allocating %d\n", error);
		}

		BTREE_UNLOCK(OTREE(slos), 0);
	}
}

/* Returns the amount of free bytes in the SLOS. */
/*
 * HACKS HACKS HACKS: Without GC what we're left with
 * is a bump allocator. We just read the size of the
 * leftover chunks to find how much space is left.
 */
int slos_freebytes(SYSCTL_HANDLER_ARGS)
{
	uint64_t freebytes;
	struct fnode_iter iter;
	uint64_t asked = 0;
	int error;

	error = fbtree_keymax_iter(STREE(&slos), &asked, &iter);
	if (error != 0) {
		return (error);
	}

	if (ITER_ISNULL(iter)) {
		printf("SLOS is full!\n");
		return (ENOSPC);
	}

	freebytes = ITER_KEY_T(iter, uint64_t);
	error = SYSCTL_OUT(req, &freebytes, sizeof(freebytes));

	return (error);
}

/*
 * Initialize the in-memory allocator state at mount time.
 */
int
slos_allocator_init(struct slos *slos)
{
	struct slos_node *offt;
	struct slos_node *sizet;
	diskptr_t ptr;
	uint64_t off;
	uint64_t total;
	int error;

	/*
	 * If epoch is -1 then this is the first time we mounted this device,
	 * this means we have to manually allocate, since we know how much space
	 * we used on disk (just the superblock array) we just take that offset
	 * and bump it to allocate.
	 */

	size_t offset = ((NUMSBS * slos->slos_sb->sb_ssize) /
			    slos->slos_sb->sb_bsize) +
	    1;
	// Checksum tree is allocated first.
	offset += 2;
	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		DEBUG1(
		    "Bootstrapping Allocator for first time startup starting at offset %lu",
		    offset);
		/*
		 * When initing the allocator, we have to start out by just
		 * bump allocating the initial setup of the trees,  we bump
		 * each tree by two, one for the inode itself, and the second
		 * for the root of the tree.
		 */
		fbtree_sysinit(slos, offset, &slos->slos_sb->sb_allocoffset);
		offset += 2;
		fbtree_sysinit(slos, offset, &slos->slos_sb->sb_allocsize);
		offset += 2;
	}

	uint64_t offbl = slos->slos_sb->sb_allocoffset.offset;
	uint64_t sizebl = slos->slos_sb->sb_allocsize.offset;

	DEBUG("Initing Allocator");
	/* Create the in-memory vnodes from the on-disk state. */
	error = slos_svpimport(slos, offbl, true, &offt);
	KASSERT(error == 0,
	    ("importing allocator offset tree failed with %d", error));
	error = slos_svpimport(slos, sizebl, true, &sizet);
	KASSERT(error == 0,
	    ("importing allocator size tree failed with %d", error));

	// This is a remount so we must free the allocator slos_nodes as they
	// are not cleaned up in the vflush as they have no associated vnode to
	// them.
	if (slos->slos_alloc.a_offset != NULL) {
		slos_vpfree(slos, slos->slos_alloc.a_offset);
	}
	slos->slos_alloc.a_offset = offt;

	if (slos->slos_alloc.a_size != NULL) {
		slos_vpfree(slos, slos->slos_alloc.a_size);
	}
	slos->slos_alloc.a_size = sizet;

	slos->slos_alloc.chunk.offset = 0;
	slos->slos_alloc.chunk.size = 0;

	// We just have to readjust the elements in the btree since we are not
	// using them for the same purpose of keeping track of data
	MPASS(offt && sizet);
	fbtree_init(offt->sn_fdev, offt->sn_tree.bt_root, sizeof(uint64_t),
	    sizeof(uint64_t), &uint64_t_comp, "Off Tree", 0, OTREE(slos));
	fbtree_reg_rootchange(OTREE(slos), &slos_generic_rc, offt);

	fbtree_init(sizet->sn_fdev, sizet->sn_tree.bt_root, sizeof(uint64_t),
	    sizeof(uint64_t), &uint64_t_comp, "Size Tree", 0, STREE(slos));
	fbtree_reg_rootchange(STREE(slos), &slos_generic_rc, sizet);

	// New tree add the initial amount allocations.  Im just making some
	// constant just makes it easier
	/*
	 * If the allocator is uninitialized, populate the trees with the
	 * initial values.
	 * TODO Error handling for fbtree_insert().
	 */
	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		DEBUG("First time start up for allocator");
		off = offset * BLKSIZE(slos);
		total = slos->slos_sb->sb_size - (offset * BLKSIZE(slos));

		KASSERT(fbtree_size(OTREE(slos)) == 0, ("Bad size\n"));
		KASSERT(fbtree_size(STREE(slos)) == 0, ("Bad size\n"));

		BTREE_LOCK(OTREE(slos), LK_EXCLUSIVE);
		BTREE_LOCK(STREE(slos), LK_EXCLUSIVE);

		fbtree_insert(OTREE(slos), &off, &total);
		fbtree_insert(STREE(slos), &total, &off);

		BTREE_UNLOCK(STREE(slos), 0);
		BTREE_UNLOCK(OTREE(slos), 0);

		/*
		 * Carve of a region from the beginning of the device.
		 * We have statically allocated some of these blocks using
		 * the userspace tool, so we retroactively log that allocation
		 * using the call below.
		 * TODO: More dynamic allocation that does exactly the
		 * allocations done?
		 */
		slos_blkalloc(slos, NEWOSDSIZE * BLKSIZE(slos), &ptr);
		DEBUG("First time start up for allocator done");
	}

	return (0);
};

/*
 * Initialize the in-memory allocator state at mount time.
 */
int
slos_allocator_uninit(struct slos *slos)
{
	slos_vpfree(slos, slos->slos_alloc.a_offset);
	slos->slos_alloc.a_offset = NULL;
	slos_vpfree(slos, slos->slos_alloc.a_size);
	slos->slos_alloc.a_size = NULL;

	return (0);
}

/*
 * Flush the allocator state to disk.
 */
int
slos_allocator_sync(struct slos *slos, struct slos_sb *newsb)
{
	int error;
	struct buf *bp;
	diskptr_t ptr;

	/* This is just arbitrary right now.  We need a better way of
	 * calculating the dirty blocks we over allocate because we need to
	 * reallocate parents as well even though they may not be dirty, we are
	 * changing the location of one or more of their children so we must
	 * also mark them as CoW
	 *
	 * This could be done by going through each of the dirtylsit (just leave
	 * nodes) crawl upwards and mark something as new copy on write and then
	 * cnt unique times you've done it. Once youve reached something that
	 * has been marked you can just stop that iteration. As you know all the
	 * parents have been marked
	 */
	int total_allocations = (FBTREE_DIRTYCNT(OTREE(slos)) * 5) +
	    (FBTREE_DIRTYCNT(STREE(slos)) * 5) + 2;

	DEBUG("Syncing Allocator");
	// Allocate and sync the btree's
	error = slos_blkalloc(slos, total_allocations * BLKSIZE(slos), &ptr);
	if (error != 0) {
		printf("Unexpected slos_blkalloc failed %d!\n", error);
		return (error);
	}
	error = fbtree_sync_withalloc(OTREE(slos), &ptr);
	MPASS(error == 0);
	error = fbtree_sync_withalloc(STREE(slos), &ptr);
	MPASS(error == 0);

	DEBUG2("off(%p), size(%p)", slos->slos_alloc.a_offset->sn_fdev,
	    slos->slos_alloc.a_size->sn_fdev);
	// Update the inodes and dirty them as well
	bp = getblk(slos->slos_alloc.a_offset->sn_fdev, ptr.offset,
	    BLKSIZE(slos), 0, 0, 0);
	MPASS(bp);
	struct slos_inode *ino = &slos->slos_alloc.a_offset->sn_ino;
	ino->ino_blk = ptr.offset;
	slos->slos_sb->sb_allocoffset = ptr;
	ino->ino_btree = (diskptr_t) { OTREE(slos)->bt_root, BLKSIZE(slos) };
	memcpy(bp->b_data, ino, sizeof(struct slos_inode));
	bwrite(bp);
	ptr.offset += 1;

	bp = getblk(slos->slos_alloc.a_size->sn_fdev, ptr.offset, BLKSIZE(slos),
	    0, 0, 0);
	MPASS(bp);
	ino = &slos->slos_alloc.a_size->sn_ino;
	ino->ino_blk = ptr.offset;
	ino->ino_btree = (diskptr_t) { STREE(slos)->bt_root, BLKSIZE(slos) };
	slos->slos_sb->sb_allocsize = ptr;
	memcpy(bp->b_data, ino, sizeof(struct slos_inode));
	bwrite(bp);

	// XXX Check how much is left and free them - but since we dont have
	// free yet leave this for later.
	return (0);
}
