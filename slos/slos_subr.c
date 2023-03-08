#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/bufobj.h>
#include <sys/dirent.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>

#include <machine/atomic.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>
#include <slos.h>
#include <slos_inode.h>
#include <slos_io.h>
#include <slsfs.h>

#include <btree.h>

#include "../slfs/slsfs_dir.h"
#include "debug.h"
#include "slos_subr.h"
#include "slsfs_buf.h"

#define SYNCERR (-1)
#define DONE (0)
#define DIRTY (1)

struct buf_ops bufops_slsfs = {
	.bop_name = "slos_bufops",
	.bop_strategy = slos_bufstrategy,
	.bop_write = slos_bufwrite,
	.bop_sync = slos_bufsync,
	.bop_bdflush = slos_bufbdflush,
};

void

/*
 * Have the in memory inode update its offset in the inode btree. This
 * operation also brings the on-disk metadata into memorya and updates it.
 */
slos_generic_rc(void *ctx, bnode_ptr p)
{
	struct slos_node *svp = (struct slos_node *)ctx;
	svp->sn_ino.ino_btree.offset = p;
	slos_update(svp);
}

/*
 * Allocate a new SLOS inode.
 */
int
slos_svpalloc(struct slos *slos, mode_t mode, uint64_t *slsidp)
{
	int slsid;
	int slsid_requested;
	int error;

	/*
	 * Get an inode number if one is not specified.
	 *
	 * The unique identifier generator is 31-bit. This unfortunately
	 * means we have to truncate the SLS IDs passed by the SLS. Since the
	 * probability of a collision is small enough, this isn't a big
	 * limitation.
	 *
	 */
	slsid_requested = OIDTOSLSID(*slsidp);
retry_create:
	if (slsid_requested == 0) {
		/* If any ID will do, we need to get one. */
		slsid = alloc_unr(slsid_unr);
		if (slsid < 0)
			return (EINVAL);
	} else {
		/* If we're going for a specific inode, it's fine if it's
		 * already there. */
		slsid = slsid_requested;
	}

	/* Create the inode. */
	error = slos_icreate(slos, (uint64_t)slsid, mode);
	if (error != 0) {
		// If they requested nothing then we should retry
		if (slsid_requested == 0) {
			goto retry_create;
		}

		/*
		 * If it already exists and we asked for this specific ID,
		 * we're calling this from the SLS and we don't care. Just
		 * return success.
		 */
		if (error == EEXIST) {
			*slsidp = slsid;
			return (0);
		}

		return (error);
	}

	*slsidp = slsid;

	return (0);
}

int
slos_setupfakedev(struct slos *slos, struct slos_node *vp)
{
	struct vnode *devvp;
	int error;

	error = getnewvnode("SLSFS Fake VNode", slos->slsfs_mount,
	    &slsfs_vnodeops, &vp->sn_fdev);
	if (error) {
		panic("Problem getting fake vnode for device\n");
	}
	devvp = vp->sn_fdev;
	/* Set up the necessary backend state to be able to do IOs to the
	 * device. */
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
slos_remove_node(
    struct vnode *dvp, struct vnode *vp, struct componentname *name)
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
 * Destroy an in-memory SLOS inode for a deleted file. The node must be already
 * dead.
 */
int
slos_destroy_node(struct slos_node *vp)
{
	KASSERT(vp->sn_status == IN_DEAD, ("destroying still active node"));
	return (0);
}

/*
 * Adjust a vnode's data to be of a specified size.
 */
int
slos_truncate(struct vnode *vp, size_t size)
{
	struct slos_node *svp = SLSVP(vp);
	int error;

	/* Dirty the inode, if anything to update the time accessed. */
	error = slos_update(svp);
	if (error != 0)
		return (error);

	if (size == svp->sn_ino.ino_size)
		return (0);

	/* Notify the VFS layer of the change. */
	svp->sn_ino.ino_size = size;
	vnode_pager_setsize(vp, svp->sn_ino.ino_size);

	/* Nuke the existing data if we are truncating the file to 0. */
	if (size == 0)
		return (bufobj_invalbuf(&vp->v_bufobj, 0, 0, 0));

	return (0);
}

/* Flush a vnode's data to the disk. */
int
slos_sync_vp(struct vnode *vp, int release)
{
	struct fbtree *tree = &SLSVP(vp)->sn_tree;
	ASSERT_VOP_LOCKED(vp, __func__);

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
slos_bufwrite(struct buf *buf)
{
	return (bufwrite(buf));
}

/* Send a SLOS buffer to disk. */
int
slos_bufsync(struct bufobj *bufobj, int waitfor)
{
	/* Add a check of whether it's dirty. */
	struct vnode *vp = bo2vnode(bufobj);
	if (SLS_ISWAL(vp))
		return (bufsync(bufobj, waitfor));

	return (slos_sync_vp(bo2vnode(bufobj), 0));
}

/* Mark a buffer as a candidate to be flushed. */
void
slos_bufbdflush(struct bufobj *bufobj, struct buf *buf)
{
	bufbdflush(bufobj, buf);
}

/*
 * Do a block IO for the buffer.
 */
void
slos_bufstrategy(struct bufobj *bo, struct buf *bp)
{
	struct vnode *vp;
	int error;

	vp = bo2vnode(bo);
	// The device is a VCHR but we still want to use vop strategy on it
	// This is to allow us to bypass using BMAP operations on the vnode
	KASSERT(vp->v_type != VCHR || (vp->v_vflag & VV_SYSTEM),
	    ("Wrong vnode in buf strategy %p", vp));
	error = VOP_STRATEGY(vp, bp);
	KASSERT(error == 0, ("VOP_STRATEGY failed"));
}
