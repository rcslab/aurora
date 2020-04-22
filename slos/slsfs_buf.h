#include <sys/param.h>
#include <sys/vnode.h>
#include <sys/buf.h>
#include <sys/ucred.h>

#include <slos_inode.h>

/*
 * Given some vnode, bcreate will create a struct buf at the given logical
 * block number (lbn), of size xfersize and allocate it and attach it to the 
 * given pointer at buf.
 */
int slsfs_bcreate(struct vnode *node, uint64_t lbn, size_t xfersize, struct buf **buf);

/*
 * Given some vnode will read in the buffer associated with the logical block 
 * number (lbn), and attach it to the pointer at buf.
 */
int slsfs_bread(struct vnode *node, uint64_t lbn, size_t size, struct ucred *cred, struct buf **buf);

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
int slsfs_lookupbln(struct slos_node *svp, uint64_t lbn,  struct fnode_iter *iter);
