#include <sys/param.h>

#include <sys/buf.h>
#include <sys/systm.h>
#include <sys/uio.h>

#include <slos.h>
#include <slos_bnode.h>
#include <slsfs.h>

#include "slos_bootalloc.h"
#include "slos_io.h"
#include "slosmm.h"

/*
 * Helper functions for getting/setting values in the bnodes.
 * Because bnodes have variable size as structs, and also
 * have variable value size, using macros to get/set these
 * would be brittle, so we have proper functions.
 */

/* Get the key of a bnode at a specific offset. */
int
bnode_getkey(struct bnode *bnode, size_t offset, uint64_t *key)
{
	uint64_t *keys;

	/* 
	 * We do have bnode_inbounds which checks if an 
	 * offset is in the bnode, but it's not for keys.
	 */
	if (offset >= bnode->size) 
	    return EINVAL;

	keys = (uint64_t *) bnode->data;
	*key = keys[offset];

	return 0;
}

/* Get the value of a bnode at a specific offset. */
int
bnode_getvalue(struct bnode *bnode, size_t offset, void *value)
{
	/* 
	 * Arrays of uint8_t allow us to do pointer 
	 * arithmetic on arrays of unknown types.
	 */
	uint8_t *values;

	if (bnode_inbounds(bnode, offset) == BNODE_OOB)
	    return EINVAL;

	/* Find the beginning of the values array. */
	values =  &bnode->data[(bnode->bsize + 1) * SLOS_KEYSIZE];

	/* Copy the bytes of the struct out. */
	memcpy(value, &values[offset * bnode->vsize], bnode->vsize);

	return 0;
}

/* Get the child pointer of a bnode at a specific offset. */
int
bnode_getptr(struct bnode *bnode, size_t offset, struct slos_diskptr *diskptr)
{
	struct slos_diskptr *ptrs;

	if (bnode_inbounds(bnode, offset) == BNODE_OOB)
	    return EINVAL;

	/* Find the beginning of the values array. */
	ptrs = (struct slos_diskptr *) &bnode->data[(bnode->bsize + 1) * SLOS_KEYSIZE];
	*diskptr = ptrs[offset];

	return 0;
}

/* Put a key in a bnode at a specific offset. */
int
bnode_putkey(struct bnode *bnode, size_t offset, uint64_t key)
{
	uint64_t *keys;

	/* 
	 * We do have bnode_inbounds which checks if an 
	 * offset is in the bnode, but it's not for keys.
	 * We _can_ add to position bnode->bsize - this
	 * happens right before we split.
	 */
	if (offset > bnode->bsize) 
	    return EINVAL;

	keys = (uint64_t *) bnode->data;
	keys[offset] = key;

	return 0;
}

/* Put the value in a bnode at a specific offset. */
int
bnode_putvalue(struct bnode *bnode, size_t offset, void *value)
{
	uint8_t *values;

	/* 
	 * Same rule as with keys - we can add 
	 * to position bnode->bsize, because
	 * we assume we'll split the bnode
	 * immediately after.
	 */
	if (offset > bnode->bsize)
	    return EINVAL;

	/* Find the beginning of the values array. */
	values = &bnode->data[(bnode->bsize + 1) * SLOS_KEYSIZE];

	/* Copy the bytes of the struct out. */
	memcpy(&values[offset * bnode->vsize], value, bnode->vsize);

	return 0;
}

/* Get the child pointer of a bnode at a specific offset. */
int
bnode_putptr(struct bnode *bnode, size_t offset, struct slos_diskptr diskptr)
{
	struct slos_diskptr *ptrs;

	if (offset >= bnode->bsize + 1)
	    return EINVAL;

	/* Find the beginning of the values array. */
	ptrs = (struct slos_diskptr *) &bnode->data[(bnode->bsize + 1) * SLOS_KEYSIZE];
	ptrs[offset] = diskptr;

	return 0;
}



/* 
 * Create an in-memory bnode. The block
 * number corresponds to the disk block reserved
 * for the bnode. Fields are initialized to legal values.
 */
struct bnode *
bnode_alloc(struct slos *slos, uint64_t blkno, uint64_t vsize, int external)
{
	struct bnode *bnode;

	bnode = malloc(slos->slos_sb->sb_bsize, M_SLOS, M_WAITOK | M_ZERO);

	bnode->blkno = blkno;
	bnode->external = external;
	bnode->vsize = vsize;
	/* 
	 * The spacing between consecutive elements of the array needs
	 * to be able to hold a disk pointer, since the values of
	 * internal bnodes need to be disk pointers themselves.
	 */
	bnode->bsize = ((slos->slos_sb->sb_bsize - sizeof(struct bnode)) / 
	     (max(vsize, sizeof(struct slos_diskptr)) + sizeof(uint64_t))) - 1;
	bnode->size = 0;
	bnode->parent = DISKPTR_BLOCK(blkno);
	bnode->magic = SLOS_BMAGIC;

	return bnode;
}


/*
 * Destroy the in-memory representation of a bnode
 * which resides in a btree whose root bnode is root.
 * This function is unrelated to the on-disk block.
 */
void
bnode_free(struct bnode *bnode)
{
	/* As with regular free, freeing a nullptr is a no-op. */
	if (bnode == NULL)
	    return;

	free(bnode, M_SLOS);
}


/* 
 * Move len entries from bnode src (starting from
 * offset srcoff) to bnode dst starting from 
 * offset dstoff).
 */
int
bnode_copydata(struct bnode *dst, struct bnode *src, 
	size_t dstoff, size_t srcoff, size_t len)
{
	struct slos_diskptr *dstptrs, *srcptrs;
	uint8_t *dstvalues, *srcvalues;
	uint64_t *dstkeys, *srckeys;
	
	/* Check if the operation requested is valid. */
	if (dst->external != src->external)
	    return EINVAL;

	if (dst->vsize != src->vsize)
	    return EINVAL;

	/* Make sure the offsets aren't out of bounds. */
	if (dstoff > dst->bsize + 1)
	    return EINVAL;

	if (srcoff > src->bsize + 1)
	    return EINVAL;

	/* Truncate data movement length to be valid. */
	if (len > dst->bsize + 1 - dstoff)
	    len = dst->bsize + 1 - dstoff;

	if (len > src->bsize + 1 - srcoff)
	    len = src->bsize + 1 - srcoff;


	dstkeys = (uint64_t *) dst->data;
	srckeys = (uint64_t *) src->data;

	memmove(&dstkeys[dstoff], 
		&srckeys[srcoff], 
		SLOS_KEYSIZE * len);

	if (dst->external == BNODE_EXTERNAL) {
	    dstvalues = &dst->data[(dst->bsize + 1) * SLOS_KEYSIZE];
	    srcvalues = &src->data[(src->bsize + 1) * SLOS_KEYSIZE];

	    memmove(&dstvalues[dst->vsize * dstoff],
		    &srcvalues[dst->vsize * srcoff], 
		    dst->vsize * len);
	} else {
	    dstptrs = (struct slos_diskptr *) &dst->data[(dst->bsize + 1) * SLOS_KEYSIZE];
	    srcptrs = (struct slos_diskptr *) &src->data[(src->bsize + 1) * SLOS_KEYSIZE];
	    memmove(&dstptrs[dstoff], 
		    &srcptrs[srcoff], 
		    sizeof(struct slos_diskptr) * len);
	}

	return 0;
}

/*
 * Shift a bnode's key/{value, child} pairs to the left 
 * by shift positions, starting from offset offset. The
 * bnode's size gets adjusted accordingly.
 */
void
bnode_shiftl(struct bnode *bnode, size_t offset, size_t shift)
{
	/* 
	 * We allow bnode->bsize + 1 temporarily, 
	 * the nodes will be resized by splitting. 
	 */

	bnode_copydata(bnode, bnode, offset, offset + shift, bnode->bsize - offset);
	bnode->size = max(bnode->size - shift, 0);
}

/*
 * Shift a bnode's key/{value, child} pairs to the right 
 * by shift positions, starting from offset offset. The
 * bnode's size gets adjusted accordingly.
 */
void
bnode_shiftr(struct bnode *bnode, size_t offset, size_t shift)
{
	/* 
	 * We allow bnode->bsize + 1 temporarily, 
	 * the nodes will be resized by splitting. 
	 */
	bnode_copydata(bnode, bnode, offset + shift, offset, bnode->bsize - offset);
	bnode->size = min(bnode->size + shift, bnode->bsize + 1);
}

/*
 * Check whether the bnodes's keys are 
 * in strictly ascending order.
 */
int
bnode_isordered(struct bnode *bnode)
{
	uint64_t left, right;
	int i;

	for (i = 0; i < bnode->size - 1; i++) {
	    /* We are defacto in bounds. */
	    bnode_getkey(bnode, i, &left);
	    bnode_getkey(bnode, i + 1, &right);

	    if (left >= right)
		return BNODE_OOO;
	}

	return BNODE_OK;
}

/*
 * Check whether the bnode's size adheres to
 * the invariants of the btree.
 */
int
bnode_issized(struct bnode *bnode) {
	/* 
	* If we are the root, we have no 
	* lower bounds for our size in most
	* cases.
	*/
	if (bnode->parent.offset == bnode->blkno) {
	    /* If we're external we only have a lower bound. */
	    if ((bnode->external == BNODE_EXTERNAL) && bnode->size <= bnode->bsize)
		return BNODE_OK;

	    /* Else we're internal, and we have a lower bound of 1. */
	    if ((bnode->external == BNODE_INTERNAL) && 
		(bnode->size > 0) && 
		(bnode->size < bnode->bsize))
		return BNODE_OK;

	    
	    return BNODE_SZERR;
	}

	/* 
	* If we're not the root, then the regular bounds apply. 
	* Since size denotes the number of keys, not children/values,
	* we have different cases for each type of bnode.
	*/

	if (bnode->external == BNODE_EXTERNAL) {
	    if (2 * bnode->size >= bnode->bsize && bnode->size <= bnode->bsize)
		return BNODE_OK;

	    return BNODE_SZERR;
	}

	/* We're internal, so use offset the bnode size by one. */
	if (2 * (bnode->size + 1) >= bnode->bsize && (bnode->size + 1) <= bnode->bsize)
	    return BNODE_OK;

	return BNODE_SZERR;
}

/*
 * Return the offset of the child in its parent node.
 */
int
bnode_parentoff(struct bnode *bnode, struct bnode *bparent)
{
	struct slos_diskptr parentptr;
	int boffset; 
	int i;

	boffset = bparent->size + 1;
	for (i = 0; i < bparent->size + 1; i++) {
	    /* No need to check, we are always in bounds. */
	    bnode_getptr(bparent, i, &parentptr);
	    if (parentptr.offset == bnode->blkno) {
		boffset = i;
		break;
	    }
	}

	return boffset;
}

void 
bnode_print(struct bnode *bnode)
{
	size_t size;
	struct slos_diskptr diskptr;
	uint8_t *value;
	uint64_t key;
	int i;

	DBUG("Bnode %lu (%p), Size %d External %s Root %s\n", bnode->blkno, bnode,
		bnode->size, bnode->external == BNODE_EXTERNAL ? "yes" : "no",
		bnode->parent.offset == bnode->blkno ? "yes" : "no");


	/* 
	 * Print the values. If the node is internal, then it has 
	 * n + 1 children for n keys.
	 */
	size = (bnode->external == BNODE_EXTERNAL) ? bnode->size : bnode->size + 1;
	if (bnode->external == BNODE_INTERNAL) {
	    /* If it's an internal node, the values are of type slos_diskptr*/

	    for (i = 0; i < size; i++) {
		bnode_getptr(bnode, i, &diskptr);
		if (i < bnode->size) {
		    bnode_getkey(bnode, i, &key);
		    DBUG("(%lu: (%lu, %lu))\t", key, diskptr.offset,
			    diskptr.size);
		} else {
		    DBUG("(NONE: (%lu, %lu))\t", diskptr.offset,
			    diskptr.size);

		}
	    }

	} else {
	    /* Since values have arbitrary types, we only attempt
	     * to print out */
	    value = malloc(bnode->vsize, M_SLOS, M_WAITOK);

	    for (i = 0; i < size; i++) {
		bnode_getkey(bnode, i, &key);
		bnode_getvalue(bnode, i, value);
		DBUG("(%lu: %lu)\t",  key, *((uint64_t *) value));
	    }

	    free(value, M_SLOS);
	} 
	DBUG("\n");
}

/*
 * Import a bnode from the OSD.
 */
int
bnode_read(struct slos *slos, daddr_t blkno, struct bnode **bnodep)
{
	struct bnode *bnode;
	int error;

	bnode = malloc(slos->slos_sb->sb_bsize, M_SLOS, M_WAITOK);
	
	/* Read the bnode from the disk. */
	error = slos_readblk(slos, blkno, bnode); 
	if (error != 0) {
	    DBUG("Readblk error\n");
	    free(bnode, M_SLOS);
	    *bnodep = NULL;
	    return error;
	}

	if (bnode->magic != SLOS_BMAGIC) {
	    DBUG("Wrong magic number\n");
	    free(bnode, M_SLOS);
	    *bnodep = NULL;
	    return EINVAL;
	}

	/* 
	 * Everything went well, 
	 * make the bnode visible. 
	 */
	*bnodep = bnode;

	return 0;
}

/*
 * Export a bnode to the OSD.
 */

int
bnode_write(struct slos *slos, struct bnode *bnode)
{
	if (bnode->magic != SLOS_BMAGIC)
	    return EINVAL;

	/* Write bnode to the disk. */
	return slos_writeblk(slos, bnode->blkno, bnode); 
}

/* 
 * Create an in-memory copy of bnode which 
 * is backed by block blkno on disk. Used when
 * an operation modifies multiple bnodes in the tree.
 * Because these bnodes always comprise a subtree,
 * what we do is create a copy of the subtree and
 * replace it atomically by modifying the parent
 * of the subtree's root to point to the new
 * version.
 */
struct bnode *
bnode_copy(struct slos *slos, uint64_t blkno, struct bnode *bnode)
{
	struct bnode *bcopy;

	bcopy = bnode_alloc(slos, blkno, bnode->vsize, bnode->external);
	bcopy->size = bnode->size;
	bcopy->parent = bnode->parent;
	bcopy->bsize = bnode->bsize;

	/*
	 * Note: Arrays of size 0 are _not_ pointers; they have size 0. 
	 * They therefore don't change the struct's size.
	 */
	memcpy(bcopy->data, bnode->data, slos->slos_sb->sb_bsize - sizeof(struct bnode));

	/* Write out the new copy. */
	bnode_write(slos, bcopy);

	return bcopy;
}


/* Check whether an offset into a bnode is valid. */
int
bnode_inbounds(struct bnode *bnode, size_t offset)
{
    if (bnode->external == BNODE_EXTERNAL)
	return (offset < bnode->size) ? BNODE_OK : BNODE_OOB;

    else 
	return (offset < bnode->size + 1) ? BNODE_OK : BNODE_OOB;
    
}

/* Get an in-memory bnode for the child of node bparent at offset. */
struct bnode *
bnode_child(struct slos *slos, struct bnode *bparent, size_t offset)
{
	struct bnode *bchild; 
	struct slos_diskptr diskptr;
	int error;

	/* External nodes do not have blocks as values. */
	if (bparent->external == BNODE_EXTERNAL)
	    return NULL;

	error = bnode_getptr(bparent, offset, &diskptr);
	if (error != 0)
	    return NULL;

	error = bnode_read(slos, diskptr.offset, &bchild);
	if (error != 0)
	    return NULL;
	
	return bchild;
}

/* 
 * Get an in-memory bnode for the parent of node bchild. 
 * Since the root of the btree is always in memory, 
 * don't create another version if it is the parent.
 * */
struct bnode *
bnode_parent(struct slos * slos, struct bnode *bchild)
{
	struct bnode *bparent; 
	int error;

	error = bnode_read(slos, bchild->parent.offset, &bparent);
	if (error != 0)
	    return NULL;
	
	return bparent;
}

/* Look for a key in a bnode. If found, return its value. */
int
bnode_search(struct bnode *bnode, uint64_t key, void *value)
{
	uint64_t low, high, mid;
	uint64_t bkey;

	/* If we have size 0, we definitely don't have the key. */
	if (bnode->size == 0)
	    return EINVAL;

	/* Get the low and high margins for binary searching. */
	low = 0;
	high = bnode->size - 1;

	while (low < high) {
	    mid = (low + high) / 2;
	    bnode_getkey(bnode, mid, &bkey);
	    if (key == bkey) {
		/* 
		 * Copy the value to the buffer provided. 
		 * We are always in bounds here.
		 */
		bnode_getvalue(bnode, mid, value);
		return 0;
	    } 

	    /* No match, adjust the ranges. */
	    if (key < bkey) {
		/* Search in the lower half of the range. */
		high = mid - 1;
	    } else {
		/* Search in the upper half of the range. */
		low = mid + 1;
	    }
	}

	/* Check if the bkey = low = mid = high. */
	mid = (low + high) / 2;
	bnode_getkey(bnode, mid, &bkey);
	if (key == bkey) {
	    bnode_getvalue(bnode, mid, value);
	    return 0;
	}

	
	return EINVAL;
}
