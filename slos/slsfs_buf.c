
#include <sys/types.h>
#include <sys/param.h>
#include <sys/buf.h>
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <vm/uma.h>

#include <machine/atomic.h>

#include <slos.h>
#include <slos_bnode.h>
#include <slos_btree.h>
#include <slos_inode.h>
#include <slos_io.h>
#include <slsfs.h>

#include "btree.h"
#include "debug.h"
#include "slsfs_buf.h"

SDT_PROVIDER_DEFINE(slsfsbuf);
SDT_PROBE_DEFINE(slsfsbuf, , , start);
SDT_PROBE_DEFINE(slsfsbuf, , , end);

/*
 * This function assume that the tree at the iter does not exist or is no in
 * the way.  It then increments the iterator to find the position of the next
 * key either the key is close and we have to truncate our size to squeeze
 * between the previos key and the next or we can fully insert ourselves into
 * the truee
 */
static int
slsfs_buf_nocollide(struct vnode *vp, struct fnode_iter *biter, uint64_t bno,
    uint64_t size, enum uio_rw rw, int gbflag, struct buf **bp)
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
		size = omin((nextkey - bno) * blksize, size);
	}

	ptr.offset = 0;
	ptr.size = size;
	ptr.epoch = EPOCH_INVAL;
	error = BTREE_LOCK(tree, LK_UPGRADE);
	if (error) {
		panic("Could not acquire lock upgrade on Btree %d", error);
	}

	error = fbtree_insert(tree, &bno, &ptr);
	if (error) {
		panic("Problem inserting into tree");
		return (error);
	}
	/*
	 * We must release the BTree here, as any getblk allocation could
	 * trigguer a buf domain flush would cause dirty buffers to flush
	 * and bstrategies to be done which are done by the current thread,
	 * so we end up getting an issue where since we are holding this lock
	 * in exclusive, we get a panic as we try to acquire the lock in shared
	 * when doing the lookup within slsfs_strategy
	 */
	ITER_RELEASE(*biter)
	size = omin(size, MAXBCACHEBUF);
	MPASS(size <= MAXBCACHEBUF);
	error = slsfs_balloc(vp, bno, size, gbflag, bp);
	if (error != 0)
		panic("Balloc failed\n");

	if (*bp == NULL)
		panic("slsfs_buf_nocollide failed to allocate\n");

	return (error);
}

int
slsfs_retrieve_buf(struct vnode *vp, uint64_t offset, uint64_t size,
    enum uio_rw rw, int gbflag, struct buf **bp)
{
	struct fnode_iter biter;
	bool covered, isaligned;
	size_t extoff;
	diskptr_t ptr;
	int error;

	uint64_t iter_key;
	struct slos_node *svp = SLSVP(vp);

	uint64_t original_size = size;
	size_t blksize = IOSIZE(svp);
	uint64_t bno = offset / blksize;

#ifdef VERBOSE
	DEBUG1("Attemping to retrieve buffer %lu bno", bno);
#endif
	error = slsfs_lookupbln(svp, bno, &biter);
	if (error) {
		DEBUG1("%d", error);
		return (error);
	}
	size = roundup(size, IOSIZE(svp));

	/* Past the end of the file, create a new dummy buffer. */
	if (ITER_ISNULL(biter)) {
		return (
		    slsfs_buf_nocollide(vp, &biter, bno, size, rw, gbflag, bp));
	}

	/* If all the buffer is a hole, return a dummy buffer. */
	iter_key = ITER_KEY_T(biter, uint64_t);
	if (!INTERSECT(biter, bno, blksize)) {
		return (
		    slsfs_buf_nocollide(vp, &biter, bno, size, rw, gbflag, bp));
	}

	/*
	 * Otherwise we overlap with an existing buffer.
	 */

	ptr = ITER_VAL_T(biter, diskptr_t);
	ITER_RELEASE(biter);

	/* Truncate the size to be down to the extent.*/
	/* XXX This manipulation may not work with 4K blocks.  */
	original_size = size;
	extoff = bno - iter_key;
	size = omin(size, ptr.size - (extoff * blksize));
	size = omin(size, MAXBCACHEBUF);

	isaligned = ((offset % blksize) == 0);
	covered = (original_size <= size);

	/*
	 * For aligned, covered writes we just grab random space since
	 * we overwrite it anyway.
	 */
	if (isaligned && covered && (rw == UIO_WRITE)) {
		*bp = getblk(vp, bno, size, 0, 0, gbflag);
		if (*bp == NULL)
			panic("LINE %d: null bp for %lu, %lu", __LINE__, bno,
			    size);
	} else {
		/* Otherwise do an actual IO. */
		error = slsfs_bread(vp, bno, size, NULL, gbflag, bp);
		if (*bp == NULL)
			panic("LINE %d: null bp for %lu, %lu", __LINE__, bno,
			    size);
	}

	return (error);
}

/*
 * Create a buffer corresponding to logical block lbn of the vnode.
 *
 * Since we put off allocation till sync time (checkpoint time) we just need to
 * keep track of logical blocks and sizes.
 *
 * NOTES:
 *
 * getblk uses a function called allocbuf to retrieve pages for the underlying
 * data that is required for it.  These pages can be mapped in or unmapped in
 * (GB_UNMAPPED), mapping in these pages to the address space is incredbly
 * expensive so should only be used if the caller requires the data from the
 * read/write in the kernel itself.
 *
 * Pages that are attached to the b_pages buffer when unmanaged have the
 * VPO_UNMANAGED flag.
 *
 * An issue occurs though on subsequent calls to the same lbn of a vp,
 * if you call a size smaller then the orginal when GB_UNMAPPED was orginally
 * called.  allocbuf is called and if the call is truncating the data, it will
 * release underlying pages, but these pages are unmanaged so this will panic
 * the kernel.  Block based file systems don't really need to worry about this
 * as they always just get a block, so pages are not released.
 *
 *
 * XXX:  Currently we only create blocks of blksize (this is just for
 * simplicity right now)
 */
int
slsfs_balloc(
    struct vnode *vp, uint64_t lbn, size_t size, int gbflag, struct buf **bp)
{
	struct buf *tempbuf = NULL;
	int error = 0;
	KASSERT(size % IOSIZE(SLSVP(vp)) == 0, ("Multiple of iosize"));
	/* Currently a hack (maybe not)  since this is the only inode we use
	 * VOP_WRITES for that is also a system node, we need to make sure that
	 * is only reads in its actual blocksize, if our first read is 64kb and
	 * our subsequent calls to getblk are 4kb then it will try to truncate
	 * our pages resulting in the attempted release of unmanaged pages
	 */
	if (vp != slos.slsfs_inodes)
		size = gbflag & GB_UNMAPPED ? MAXBCACHEBUF : size;
	else
		KASSERT(
		    BLKSIZE(&slos) == size, ("invalid size request %lu", size));

	tempbuf = getblk(vp, lbn, size, 0, 0, gbflag);
	if (tempbuf == NULL) {
		*bp = NULL;
		return (EIO);
	}

	vfs_bio_clrbuf(tempbuf);
	tempbuf->b_blkno = (daddr_t)(-1);

	*bp = tempbuf;

	return (error);
}

/*
 * Read a block corresponding to the vnode from the buffer cache.
 */
/* I love bread  - Me, 2020 */
int
slsfs_bread(struct vnode *vp, uint64_t lbn, size_t size, struct ucred *cred,
    int flags, struct buf **buf)
{
	int error;

#ifdef VERBOSE
	DEBUG3("Reading block at %lx of size %lu for node %p", lbn, size, vp);
#endif
	error = breadn_flags(vp, lbn, size, NULL, NULL, 0, curthread->td_ucred,
	    flags, NULL, buf);
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
	uint64_t size;
	// If we are dirtying a buf thats already que'd for a write we should
	// not signal another bawrite as the system will panic wondering why we
	if (buf->b_flags & B_DELWRI) {
		bqrelse(buf);
		return;
	}

	/* Get the value before we lose ownership of the buffer. */
	size = buf->b_bufsize;

	/* Be aggressive and start the IO immediately. */
	buf->b_flags |= B_CLUSTEROK;
	bawrite(buf);

	atomic_add_64(&slos.slos_sb->sb_used, size / BLKSIZE(&slos));

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
	if (!(buf->b_flags & B_MANAGED)) {
		bremfree(buf);
	}
	buf->b_flags &= ~(B_MANAGED);
	return bwrite(buf);
}

/*
 * Find the logically largest extent that starts before the block number
 * provided.
 */
int
slsfs_lookupbln(struct slos_node *svp, uint64_t lbn, struct fnode_iter *iter)
{
	int error;
	uint64_t key = lbn;
	BTREE_LOCK(&svp->sn_tree, LK_SHARED);
	error = fbtree_keymin_iter(&svp->sn_tree, &key, iter);

	return (error);
}
