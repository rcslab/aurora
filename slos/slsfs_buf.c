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
 * Create a buffer corresponding to logical block lbn of the vnode.
 *
 * Since we put off allocation till sync time (checkpoint time) we just need to
 * keep track of logical blocks and sizes.
 *
 * XXX:  Currently we only create blocks of blksize (this is just for
 * simplicity right now)
 */
int
slsfs_bcreate(struct vnode *vp, uint64_t lbn, size_t size, struct fnode_iter *iter, struct buf **bp)
{
	struct buf *tempbuf = NULL;
	diskptr_t ptr;
	struct slos_node *svp = SLSVP(vp);
	int error = 0;

	size = roundup(size, IOSIZE(svp));
	DBUG("Creating block at %lu of size %lu for node %p\n", lbn, size, vp);

	/*
         * Do the necessary bookkeeping in the SLOS.
         *
	 * We use the recentry data structure as currently the record btrees
	 * store recentrys which are represent their disk location and offset
	 * and len in the block.
	 */
	ptr.offset = 0;
	ptr.size = size;
	if (iter == NULL) {
		BTREE_LOCK(&svp->sn_tree, LK_EXCLUSIVE);
		error = fbtree_insert(&svp->sn_tree, &lbn, &ptr);
		BTREE_UNLOCK(&svp->sn_tree, 0);
	} else {
		error = fnode_insert(iter->it_node, &lbn, &ptr);
	}

	if (error == ROOTCHANGE) {
		svp->sn_ino.ino_btree = (diskptr_t){ svp->sn_tree.bt_root, IOSIZE(svp) };
	} else if (error) {
		DBUG("Problem with fbtree insert\n");
		return (error);
	}

	DBUG("Getting block\n");
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
