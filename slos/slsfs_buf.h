#ifndef _SLSFS_BUF_H_
#define _SLSFS_BUF_H_

#define ENDPOINT(iter, blksize)                    \
	(ITER_KEY_T((iter), uint64_t) * blksize) + \
	    (ITER_VAL_T((iter), diskptr_t).size)
#define INTERSECT(leftiter, right, blksize) \
	(ENDPOINT(leftiter, blksize) > (right * blksize))

/*
 * Given some vnode, bcreate will create a struct buf at the given logical
 * block number (lbn), of size xfersize and allocate it and attach it to the
 * given pointer at buf.
 */
int slsfs_balloc(struct vnode *node, uint64_t lbn, size_t xfersize, int gbflag,
    struct buf **buf);

int slsfs_retrieve_buf(struct vnode *vp, uint64_t offset, uint64_t size,
    enum uio_rw rw, int gbflag, struct buf **bp);

/*
 * Given some vnode will read in the buffer associated with the logical block
 * number (lbn), and attach it to the pointer at buf.
 */
int slsfs_bread(struct vnode *node, uint64_t lbn, size_t size,
    struct ucred *cred, int flags, struct buf **buf);

int slsfs_devbread(
    struct slos *slos, uint64_t lbn, size_t size, struct buf **bpp);

void slsfs_bdirty(struct buf *buf);
int slsfs_bundirty(struct buf *buf);

/*
 * Lookup Logical Block Number
 * Checks whether a specific node has a logical block number associated with
 * it. This function is used to figure whether to issue a bcreate or a bread
 *
 * On success will return a physical block number of >= 0 (pbn)
 *
 * On failure will return a physical block number of -1
 */
int slsfs_lookupbln(
    struct slos_node *svp, uint64_t lbn, struct fnode_iter *iter);

#endif /* _SLSFS_BUF_H_ */
