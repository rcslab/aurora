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
slsfs_buf_nocollide(struct vnode *vp, struct fnode_iter *biter, uint64_t bno, uint64_t size, 
	enum uio_rw rw, int gbflag, struct buf **bp)
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
	ptr.epoch = 0;
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
	MPASS(MAXBCACHEBUF == 65536);
	MPASS(size <= MAXBCACHEBUF);
	error = slsfs_balloc(vp, bno, size, gbflag, bp);
	return (error);
}

int
slsfs_retrieve_buf(struct vnode *vp, uint64_t offset, uint64_t size, enum uio_rw rw, int gbflag, struct buf **bp)
{
	struct fnode_iter biter;
	diskptr_t ptr;
	int error;
	bool covered;

	struct slos_node *svp = SLSVP(vp);
	uint64_t originalsize = size;
	size_t blksize = IOSIZE(svp);
	bool isaligned = (offset % blksize) == 0;
	uint64_t bno = offset / blksize;

	DEBUG1("Attemping to retrieve buffer %lu bno", bno);
	error = slsfs_lookupbln(svp, bno, &biter);
	if (error) {
		DEBUG1("%d", error);
		return (error);
	}
	size = roundup(size, IOSIZE(svp));
	if (ITER_ISNULL(biter)) {
		error = slsfs_buf_nocollide(vp, &biter, bno, size, rw, gbflag, bp);
		if (error) {
			panic("Problem with no collide case");
		}
	} else {
		uint64_t iter_key = ITER_KEY_T(biter, uint64_t);
		// There is a key less then us
		if (iter_key != bno) {
			// We intersect so need to read off the block on disk
			if (INTERSECT(biter, bno, blksize)) {
				size = ENDPOINT(biter, blksize) - (bno * blksize);
				size = omin(size, MAXBCACHEBUF);
				covered  = size <= originalsize;
				ITER_RELEASE(biter);
				if (isaligned && covered && (rw == UIO_WRITE)) {
					SDT_PROBE0(slsfsbuf, , , start);
					*bp = getblk(vp, bno, size, 0, 0, gbflag);
					SDT_PROBE0(slsfsbuf, , , end);
				} else {
					error = slsfs_bread(vp, bno, size, NULL, gbflag, bp);
				}
			} else {
				error = slsfs_buf_nocollide(vp, &biter, bno, size, rw, gbflag, bp);
			}
		} else {
			ptr = ITER_VAL_T(biter, diskptr_t);
			size = omin(ptr.size, MAXBCACHEBUF);
			covered  = size <= originalsize;
			ITER_RELEASE(biter);
			if (isaligned && covered && (rw == UIO_WRITE)) {
				*bp = getblk(vp, bno, size, 0, 0, gbflag);
			} else {
				error = slsfs_bread(vp, bno, size, NULL, gbflag, bp);
			}
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
slsfs_balloc(struct vnode *vp, uint64_t lbn, size_t size, int gbflag, struct buf **bp)
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
	if (vp != slos.slsfs_inodes) {
		size = gbflag & GB_UNMAPPED ? MAXBCACHEBUF : size;
	} else {
		if (BLKSIZE(&slos) != size) {
			printf("%lu %lu\n", size, UINT64_MAX);
			panic("what");
		}
	}
	tempbuf = getblk(vp, lbn, size, 0, 0, gbflag);
	if (tempbuf) {
		vfs_bio_clrbuf(tempbuf);
		tempbuf->b_blkno = (daddr_t)(-1);
	} else {
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
slsfs_bread(struct vnode *vp, uint64_t lbn, size_t size, struct ucred *cred, int flags, struct buf **buf)
{
	int error;

#ifdef VERBOSE
	DEBUG3("Reading block at %lx of size %lu for node %p", lbn, size, vp);
#endif
	error = breadn_flags(vp, lbn, size, NULL, NULL, 0, curthread->td_ucred, flags,
		NULL, buf);
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

	/* Be aggressive and start the IO immediately. */
	buf->b_flags |= B_CLUSTEROK;
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
	if (!(buf->b_flags & B_MANAGED)) {
		bremfree(buf);
	}
	buf->b_flags &= ~(B_MANAGED);
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
	BTREE_LOCK(&svp->sn_tree, LK_SHARED);
	error = fbtree_keymin_iter(&svp->sn_tree, &key, iter);

	return (error);
}
