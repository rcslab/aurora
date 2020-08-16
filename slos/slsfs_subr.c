#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/namei.h>
#include <sys/rwlock.h>
#include <sys/bio.h>
#include <vm/vm.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <machine/atomic.h>

#include <slos.h>
#include <slos_io.h>
#include <slos_record.h>
#include <slosmm.h>
#include <btree.h>

#include "slsfs.h"
#include "slsfs_subr.h"
#include "slsfs_dir.h"
#include "slsfs_buf.h"

#define SYNCERR (-1)
#define DONE (0)
#define DIRTY (1)

struct buf_ops bufops_slsfs = {
	.bop_name	=   "slsfs_bufops",
	.bop_strategy	=   slsfs_bufstrategy,
	.bop_write	=   slsfs_bufwrite,
	.bop_sync	=   slsfs_bufsync,
	.bop_bdflush	=   slsfs_bufbdflush,
};

void
slsfs_generic_rc(void *ctx, bnode_ptr p)
{
	struct slos_node *svp = (struct slos_node *)ctx;
	svp->sn_ino.ino_btree.offset = p;
	slos_update(svp);
}

/*
 * Allocate a new SLOS inode.
 */
int
slsfs_new_node(struct slos *slos, mode_t mode, uint64_t *slsidp)
{
	int slsid;
	int slsid_requested;
	int error;

	/*
	 * Get an inode number if one is not specified.
	 *
	 * The unique identifier generator is 31-bit. This unfortunately
	 * means we have to truncate the SLS IDs passed by the SLS. Since the
	 * probability of a collision is small enough, this isn't a big limitation.
	 *
	 */
	slsid_requested = OIDTOSLSID(*slsidp);
	if (slsid_requested == 0) {
		/* If any ID will do, we need to get one. */
		slsid = alloc_unr(slsid_unr);
		if (slsid < 0)
			return (EINVAL);
	} else {
		/* If we're going for a specific inode, it's fine if it's already there. */
		slsid = alloc_unr_specific(slsid_unr, slsid_requested);
		if (slsid < 0) {
			*slsidp = slsid_requested;
			return (0);
		}
	}

	/* Create the inode. */
	error = slos_icreate(slos, (uint64_t) slsid, mode);
	if (error != 0) {
		*slsidp = 0;
		return (error);
	}

	*slsidp = slsid;

	return (0);
}

int
slsfs_setupfakedev(struct slos *slos, struct slos_node *vp)
{
	struct vnode *devvp;
	int error;

	error = getnewvnode("SLSFS Fake VNode", slos->slsfs_mount, &sls_vnodeops, &vp->sn_fdev);
	if (error) {
		panic("Problem getting fake vnode for device\n");
	}
	devvp = vp->sn_fdev;
	/* Set up the necessary backend state to be able to do IOs to the device. */
	devvp->v_bufobj.bo_ops = &bufops_slsfs;
	devvp->v_bufobj.bo_bsize = slos->slos_sb->sb_bsize;
	devvp->v_type = VCHR;
	devvp->v_data = vp;
	devvp->v_vflag |= VV_SYSTEM;
	DEBUG("Setup fake device");

	return (0);
}


/*
 * Unlink an inode of a SLOS file from the directory tree.
 */
int
slsfs_remove_node(struct vnode *dvp, struct vnode *vp, struct componentname *name)
{
	int error;

	/* If the vnode is reachable from the root mount, unlink it. */
	error = slsfs_unlink_dir(dvp, vp, name);
	if (error != 0) {
		return error;
	}

	return (0);
}

/*
 * Destroy an in-memory SLOS inode for a deleted file. The node must be already dead.
 */
int
slsfs_destroy_node(struct slos_node *vp)
{
	KASSERT(vp->sn_status == SLOS_VDEAD, ("destroying still active node"));
	return (0);
}

/*
 * Retrieve an in-memory SLOS inode, or create one from disk if not present.
 */
int
slsfs_get_node(struct slos *slos, uint64_t slsid, struct slos_node **spp)
{
	struct slos_node *sp;

	sp = slos_iopen(slos, OIDTOSLSID(slsid));
	if (sp != NULL) {
		*spp = sp;
		return (0);
	}

	return (EINVAL);
}

/*
 * Adjust a vnode's data to be of a specified size.
 */
int
slsfs_truncate(struct vnode *vp, size_t size)
{
	struct bufobj *bo;
	struct slos_node *svp = SLSVP(vp);
	size_t filesize = svp->sn_ino.ino_size;
	bo = &vp->v_bufobj;
	if (size == 0) {
		return bufobj_invalbuf(bo, 0, 0, 0);
	} else if (size > filesize) {
		svp->sn_ino.ino_size = size;
		vnode_pager_setsize(vp, svp->sn_ino.ino_size);
	} else {
		// Maybe we should jsut set the size anyway for now?
		// svp->sn_ino.ino_size = size;
		return ENOSYS;
	}

	return (0);
}

/* Flush a vnode's data to the disk. */
int
slsfs_sync_vp(struct vnode *vp, int release)
{
	struct fbtree *tree  = &SLSVP(vp)->sn_tree;
	vn_fsync_buf(vp, 0);
	fbtree_sync(tree);

	/*
	 * Trying to update the time on the vnode holding the inodes
	 * dirties it which means we have to sync it to disk again,
	 * which means we need to update the time again. Avoid this
	 * infinite loop by breaking out.
	 */
	SLSVP(vp)->sn_status &= ~(SLOS_DIRTY);
	return (0);
}

/* Write a SLOS buffer to disk. */
int
slsfs_bufwrite(struct buf *buf)
{
	return (bufwrite(buf));
}

/* Send a SLOS buffer to disk. */
int
slsfs_bufsync(struct bufobj *bufobj, int waitfor)
{
	/* Add a check of whether it's dirty. */
	return (slsfs_sync_vp(bo2vnode(bufobj), 0));
}

/* Mark a buffer as a candidate to be flushed. */
void
slsfs_bufbdflush(struct bufobj *bufobj, struct buf *buf)
{
	bufbdflush(bufobj, buf);
}

/*
 * Do a block IO for the buffer.
 */
void
slsfs_bufstrategy(struct bufobj *bo, struct buf *bp)
{
	struct vnode *vp;
	int error;

	vp = bp->b_vp;
	// The device is a VCHR but we still want to use vop strategy on it
	// This is to allow us to bypass using BMAP operations on the vnode
	KASSERT(vp->v_type != VCHR || (vp->v_vflag & VV_SYSTEM), ("Wrong vnode in buf strategy %p", vp));
	error  = VOP_STRATEGY(vp, bp);
	KASSERT(error == 0, ("VOP_STRATEGY failed"));
}
