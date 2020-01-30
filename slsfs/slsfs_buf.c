#include <sys/types.h>

#include <slsfs.h>
#include <slos_record.h>
#include <slos_io.h>
#include <slos_btree.h>
#include <slos_bnode.h>

#include "slsfs_buf.h"
#include "../slos/slosmm.h"
#include "../slos/slos_alloc.h"

/*
 * Buffer Create
 *
 * Since we put off allocation till sync time (checkpoint time) we just need to 
 * keep track of logical blocks and sizes.  
 *
 * XXX:  Currently we only create blocks of blksize (this is just for 
 * simplicity right now)
 */
int 
slsfs_bcreate(struct vnode *vp, uint64_t lbn, size_t size, struct buf **buf) 
{
	struct buf *tempbuf;
	struct btree *tree;
	struct slos_record *rec;
	struct slos_recentry tempsize;
	int error = 0;

	/*
	 * We use the recentry data structure as currently the record btrees 
	 * store recentrys which are represent their disk location and offset 
	 * and len in the block. 
	 */
	tempsize.len = size;
	tempsize.offset = lbn;

	struct slos_node *svp = SLSVP(vp);
	size_t blksize = IOSIZE(svp);

	KASSERT(size == blksize, ("Currently we only accept sizes of blksize"));

	error = slos_firstrec(svp, &rec);
	if (error) {
		return (error);
	} 

	tree = btree_init(svp->sn_slos, rec->rec_data.offset, ALLOCMAIN);
	DBUG("Writing key/size pair to vnode %lu/%lu\n", lbn, size);
	error = btree_insert(tree, lbn, &tempsize);
	btree_destroy(tree);

	if (error) {
		return (error);
	}

	tempbuf = getblk(vp,  lbn, blksize, 0, 0, 0);
	if (tempbuf) {
		vfs_bio_clrbuf(tempbuf);
		tempbuf->b_blkno = (daddr_t)(-1);
	} else {
		error = EIO;
	}

	*buf = tempbuf;
	return (error);
}

/* I love bread  - Me, 2020 */
int 
slsfs_bread(struct vnode *vp, uint64_t lbn, struct ucred *cred, struct buf **buf)
{
	int error;

	size_t blksize = IOSIZE(SLSVP(vp));

	error = bread(vp, lbn, blksize, cred, buf);

	if (error) {
		return (error);
	}

	return (0);
}

void
slsfs_bdirty(struct buf *buf)
{
	/*
	* This B_MANAGED tells the buffer cache to not release our buffer to 
	* any queue when using bqrelse or brelse.  To be able to invalidate or 
	* fully release a buffer this flag must be unset
	*/
	buf->b_flags |= B_MANAGED;
	bdwrite(buf);
}

/*
 * Assumes buf is locked - bwrite will unlock and release buffer 
 */
int 
slsfs_bundirty(struct buf *buf)
{
	buf->b_flags &= ~(B_ASYNC | B_INVAL | B_MANAGED);
	return bwrite(buf);
}


int 
slsfs_lookupbln(struct slos_node *svp, uint64_t lbn,  uint64_t * pbn)
{
	struct slos_record *rec;
	struct slos_recentry size;
	uint64_t rno;
	int error; 

	uint64_t key = lbn;

	error = slos_firstrec(svp, &rec);
	if (error == EINVAL) {
		error = slos_rcreate(svp, SLOSREC_DATA, &rno);
		if (error) {
			DBUG("Error creating record\n");
			return (error);
		}

		*pbn = -1;
		return (0);
	} else if (error) {
		DBUG("Error seraching for first record\n");
		return (error);
	}

	struct btree * tree = btree_init(svp->sn_slos, rec->rec_data.offset, ALLOCMAIN);
	error = btree_keymin(tree, &key, &size);
	btree_destroy(tree);
	if (error == EINVAL) {
		*pbn = -1;
		return (0);
	} 
	if (error && error != EINVAL) {
		return (error);
	}
	DBUG("Found Offset/Size pair - %lu/%lu\n", key, size.len);
	*pbn = key;

	return (0); 
}
