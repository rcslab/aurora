#include <sys/types.h>
#include <machine/atomic.h>

#include <slsfs.h>
#include <slos_record.h>
#include <slos_io.h>
#include <slos_btree.h>
#include <slos_bnode.h>
#include <slosmm.h>

#include "slsfs_buf.h"
#include "btree.h"



/*
 * This function assume that the tree at the iter does not exist or is no in 
 * the way.  It then increments the iterator to find the position of the next 
 * key either the key is close and we have to truncate our size to squeeze 
 * between the previos key and the next or we can fully insert ourselves into 
 * the truee
 */
static int
slsfs_buf_nocollide(struct vnode *vp, struct fnode_iter *biter, uint64_t bno, uint64_t size, struct buf **bp)
{
	uint64_t nextkey;
	int error;
	struct slos_node *svp = SLSVP(vp);
	struct fbtree *tree = &svp->sn_tree;
	uint64_t blksize = IOSIZE(svp);
	diskptr_t ptr;

	ITER_NEXT(*biter);
	// First key
	if (!ITER_ISNULL(*biter)) {
		// Fill the whole upto the key or the full size
		nextkey = ITER_KEY_T(*biter, uint64_t);
		size = omin((nextkey * blksize) - (bno * blksize), size);
	}

	ptr.offset = 0;
	ptr.size = size;
	error = fbtree_insert(tree, &bno, &ptr);
	if (error == ROOTCHANGE) {
		svp->sn_ino.ino_btree = (diskptr_t){ svp->sn_tree.bt_root, IOSIZE(svp) };
	} else if (error) {
		DBUG("Problem with fbtree insert\n");
		return (error);
	}
	size = omin(size, MAXBCACHEBUF);
	error = slsfs_balloc(vp, bno, size, bp);
	ITER_RELEASE(*biter)
	return (error);
}

int
slsfs_retrieve_buf(struct vnode *vp, uint64_t offset, uint64_t size, struct buf **bp)
{
	struct fnode_iter biter;
	diskptr_t ptr;
	int error;

	struct slos_node *svp = SLSVP(vp);
	size_t blksize = IOSIZE(svp);
	uint64_t bno = offset / blksize;

	DBUG("Attemping to retrieve buffer %lu bno\n", bno);
	error = slsfs_lookupbln(svp, bno, &biter);
	if (error) {
		DBUG("%d\n", error);
		return (error);
	}
	size = roundup(size, IOSIZE(svp));
	DBUG("Size of possible retrieved buf %lu\n", size);
	if (ITER_ISNULL(biter)) {
		DBUG("No key smaller than %lu\n", bno);
		error = slsfs_buf_nocollide(vp, &biter, bno, size, bp);
		if (error) {
			panic("Problem with no collide case");
		}
	} else {
		uint64_t iter_key = ITER_KEY_T(biter, uint64_t);
		// There is a key less then us
		if (iter_key != bno) {
			// We intersect so need to read off the block on disk
			if (INTERSECT(biter, bno, blksize)) {
				DBUG("ENPOINT : %lu\n", ENDPOINT(biter, blksize));
				DBUG("BLKSIZE %lu\n", blksize);
				size = ENDPOINT(biter, blksize) - (bno * blksize);
				ITER_RELEASE(biter);
				size = omin(size, MAXBCACHEBUF);
				DBUG("Intersecting keys for bno %lu : %lu\n", bno, size);
				error = slsfs_bread(vp, bno, size, NULL, bp);
			// We do not intersect
			} else {
				DBUG("Key exists but not colliding %lu\n", size);
				error = slsfs_buf_nocollide(vp, &biter, bno, size, bp);
			}
		} else {
			ptr = ITER_VAL_T(biter, diskptr_t);
			ITER_RELEASE(biter);
			DBUG("Key exists reading size %lu\n", ptr.size);
			size = omin(ptr.size, MAXBCACHEBUF);
			error = slsfs_bread(vp, bno, size, NULL, bp);
		}
	}

	return (error);
}


/*
 * Create a buffer corresponding to logical block lbn of the vnode.
 *
 * Since we put off allocation till sync time (checkpoint time) we just need to
 * keep track of logical blocks and sizes.
 *
 * XXX:  Currently we only create blocks of blksize (this is just for
 * simplicity right now)
 */
int
slsfs_balloc(struct vnode *vp, uint64_t lbn, size_t size, struct buf **bp)
{
	struct buf *tempbuf = NULL;
	int error = 0;
	KASSERT(size % IOSIZE(SLSVP(vp)) == 0, ("Multiple of iosize"));
	/* Actually allocate the block in the buffer cache. */
	tempbuf = getblk(vp, lbn, size, 0, 0, 0);
	if (tempbuf) {
		DBUG("Cleaning block buf\n");
		vfs_bio_clrbuf(tempbuf);
		tempbuf->b_blkno = (daddr_t)(-1);
	} else {
		DBUG("ERROR\n");
		error = EIO;
	}

	*bp = tempbuf;

	return (error);
}

/*
 * Read a block corresponding to the vnode from the buffer cache.
 */
/* I love bread  - Me, 2020 */
int
slsfs_bread(struct vnode *vp, uint64_t lbn, size_t size, struct ucred *cred, struct buf **buf)
{
	int error;

	DBUG("Reading block at %lx of size %lu for node %p\n", lbn, size, vp);
	error = bread(vp, lbn, size, curthread->td_ucred, buf);
	if (error != 0)
		return (error);

	return (0);
}

/*
 * Mark a buffer as dirty, initializing its flush to disk.
 */
void
slsfs_bdirty(struct buf *buf)
{
	// If we are dirtying a buf thats already que'd for a write we should
	// not signal another bawrite as the system will panic wondering why we 
	if (buf->b_flags & B_DELWRI) {
		bqrelse(buf);
		return;
	}

	buf->b_flags |= B_CLUSTEROK;

        /* Be aggressive and start the IO immediately. */
	bawrite(buf);
	return;
}

/*
 * Synchronously flush a buffer out.
 *
 * Assumes buf is locked - bwrite will unlock and release the buffer.
 */
int
slsfs_bundirty(struct buf *buf)
{
	buf->b_flags &= ~(B_INVAL | B_CACHE);
	bremfree(buf);
	return bwrite(buf);
}


/*
 * Find the logically largest extent that starts before the block number provided.
 */
int
slsfs_lookupbln(struct slos_node *svp, uint64_t lbn,  struct fnode_iter *iter)
{
	int error;
	uint64_t key = lbn;
	BTREE_LOCK(&svp->sn_tree, LK_EXCLUSIVE);
	error = fbtree_keymin_iter(&svp->sn_tree, &key, iter);

	return (error);
}
