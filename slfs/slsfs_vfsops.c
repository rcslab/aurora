#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/extattr.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/lockf.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/pctrie.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_page.h>

#include <machine/param.h>
#include <machine/vmparam.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>
#include <slos.h>
#include <slos_btree.h>
#include <slos_inode.h>
#include <slos_io.h>
#include <slsfs.h>

#include <btree.h>

#include "debug.h"
#include "slos_alloc.h"
#include "slos_subr.h"
#include "slsfs_buf.h"
#include "slsfs_dir.h"

static MALLOC_DEFINE(M_SLSFS, "slsfs_mount", "SLSFS mount structures");

static vfs_root_t slsfs_root;
static vfs_statfs_t slsfs_statfs;
static vfs_vget_t slsfs_vget;
static vfs_sync_t slsfs_sync;

extern struct buf_ops bufops_slsfs;

static const char *slsfs_opts[] = { "from" };

// sysctl variables
int checksum_enabled = 0;

/*
 * Register the Aurora filesystem type with the kernel.
 */
static int
slsfs_init(struct vfsconf *vfsp)
{
	/* Setup slos structures */
	slos_init();

	fnode_zone = uma_zcreate("Btree Fnode Slabs", sizeof(struct fnode),
	    &fnode_construct, &fnode_deconstruct, NULL, NULL, UMA_ALIGN_PTR, 0);
	if (fnode_zone == NULL) {
		slos_uninit();
		return (ENOMEM);
	}

	fnode_trie_zone = uma_zcreate("Btree Fnode Trie Slabs",
	    pctrie_node_size(), NULL, NULL, pctrie_zone_init, NULL,
	    UMA_ALIGN_PTR, UMA_ZONE_NOFREE);
	if (fnode_trie_zone == NULL) {
		uma_zdestroy(fnode_zone);
		slos_uninit();
		return (ENOMEM);
	}

	return (0);
}

/*
 * Unregister the Aurora filesystem type from the kernel.
 */
static int
slsfs_uninit(struct vfsconf *vfsp)
{
	/* Wait for anyone who still has the lock. */
	SLOS_LOCK(&slos);
	KASSERT(slos_getstate(&slos) == SLOS_UNMOUNTED,
	    ("destroying SLOS with state %d", slos_getstate(&slos)));

	SLOS_UNLOCK(&slos);

	uma_zdestroy(fnode_zone);
	uma_zdestroy(fnode_trie_zone);
	slos_uninit();

	return (0);
}

/*
 * Called after allocator tree and checksum tree have been inited.
 * This function just checks if the mount is a fresh mount (by checking the
 * epoch number).
 *
 * Then fetches the vnode.  Since everything else has been initialized we
 * can fetch it normally. We then try and create the root directory of the
 * filesystem and are fine if it fails (means it already exists).
 */
static int
slsfs_inodes_init(struct mount *mp, struct slos *slos)
{
	int error;

	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		DEBUG("Initializing root inode");
		error = initialize_inode(
		    slos, SLOS_INODES_ROOT, &slos->slos_sb->sb_root);
		MPASS(error == 0);
	}
	/* Create the vnode for the inode root. */
	DEBUG1("Initing the root inode %lu", slos->slos_sb->sb_root.offset);
	error = slsfs_vget(mp, SLOS_INODES_ROOT, 0, &slos->slsfs_inodes);
	if (error) {
		panic("Issue trying to find root node on init");
		return (error);
	}
	VOP_UNLOCK(slos->slsfs_inodes, 0);

	/* Create the filesystem root. */
	error = slos_icreate(slos, SLOS_ROOT_INODE, MAKEIMODE(VDIR, S_IRWXU));
	if (error == EEXIST) {
		DEBUG("Already exists");
	} else if (error) {
		return (error);
	}

	return (0);
}

static int
slsfs_checksumtree_init(struct slos *slos)
{
	diskptr_t ptr;
	int error = 0;

	size_t offset = ((NUMSBS * slos->slos_sb->sb_ssize) /
			    slos->slos_sb->sb_bsize) +
	    1;
	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		MPASS(error == 0);
		error = fbtree_sysinit(slos, offset, &ptr);
		DEBUG1("Initializing checksum tree %lu", ptr.offset);
		MPASS(error == 0);
	} else {
		ptr = slos->slos_sb->sb_cksumtree;
	}

	// In case of remounting of a snapshot
	if (slos->slos_cktree != NULL) {
		slos_vpfree(slos, slos->slos_cktree);
	}

	DEBUG1("Loading Checksum tree %lu", ptr.offset);
	error = slos_svpimport(slos, ptr.offset, true, &slos->slos_cktree);
	KASSERT(error == 0, ("importing checksum tree failed with %d", error));

	fbtree_init(slos->slos_cktree->sn_fdev,
	    slos->slos_cktree->sn_tree.bt_root, sizeof(uint64_t),
	    sizeof(uint32_t), &uint64_t_comp, "Checksum tree", 0,
	    &slos->slos_cktree->sn_tree);
	KASSERT(
	    slos->slos_cktree != NULL, ("could not initialize checksum tree"));

	fbtree_reg_rootchange(
	    &slos->slos_cktree->sn_tree, &slsfs_root_rc, slos->slos_cktree);
	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		MPASS(fbtree_size(&slos->slos_cktree->sn_tree) == 0);
	}

	return (0);
}

static int
slsfs_startupfs(struct mount *mp)
{
	int error;
	struct slsfsmount *smp = mp->mnt_data;

	if (smp->sp_index == (-1)) {
		DEBUG("SLOS Read in Super");
		error = slos_sbread(&slos);
		if (error != 0) {
			DEBUG1("ERROR: slos_sbread failed with %d", error);
			return (error);
		}
	} else {
		if (slos.slos_sb == NULL)
			slos.slos_sb = malloc(
			    sizeof(struct slos_sb), M_SLOS_SB, M_WAITOK);
		error = slos_sbat(&slos, smp->sp_index, slos.slos_sb);
		if (error != 0) {
			free(slos.slos_sb, M_SLOS_SB);
			return (error);
		}
	}

	if (slos.slos_tq == NULL) {
		slos.slos_tq = taskqueue_create("SLOS Taskqueue", M_WAITOK,
		    taskqueue_thread_enqueue, &slos.slos_tq);
		if (slos.slos_tq == NULL) {
			panic("Problem creating taskqueue");
		}
		DEBUG1("Creating taskqueue %p", slos.slos_tq);
	}
	/*
	 * Initialize in memory the allocator and the vnode used for inode
	 * bookkeeping.
	 */

	slsfs_checksumtree_init(&slos);
	slos_allocator_init(&slos);
	slsfs_inodes_init(mp, &slos);

	/*
	 * Start the threads, probably should have a sysctl to define number of
	 * threads here.
	 */
	error = taskqueue_start_threads(
	    &slos.slos_tq, 10, PVM, "SLOS Taskqueue Threads");
	if (error) {
		panic("%d issue starting taskqueue", error);
	}
	DEBUG("SLOS Loaded.");

	return (error);
}

/*
 * Create an in-memory representation of the SLOS.
 */
static int
slsfs_create_slos(struct mount *mp, struct vnode *devvp)
{
	int error;

	cv_init(&slos.slsfs_sync_cv, "SLSFS Syncer CV");
	mtx_init(&slos.slsfs_sync_lk, "syncer lock", NULL, MTX_DEF);
	slos.slsfs_dirtybufcnt = 0;
	slos.slsfs_syncing = 0;
	slos.slsfs_mount = mp;

	slos.slos_vp = devvp;

	/* Hook up the SLOS into the GEOM provider for the backing device. */
	g_topology_lock();
	error = g_vfs_open(devvp, &slos.slos_cp, "slsfs", 1);
	if (error) {
		printf("Error in opening GEOM vfs");
		g_topology_unlock();
		goto error;
	}
	slos.slos_pp = g_dev_getprovider(devvp->v_rdev);
	g_topology_unlock();

	error = slsfs_startupfs(mp);
	if (error) {
		g_topology_lock();
		g_vfs_close(slos.slos_cp);
		g_topology_unlock();
		goto error;
	}

	return (0);

error:
	cv_destroy(&slos.slsfs_sync_cv);
	mtx_destroy(&slos.slsfs_sync_lk);
	return (error);
}

/*
 * Mount a device as a SLOS, and create an in-memory representation.
 */
static int
slsfs_mount_device(
    struct vnode *devvp, struct mount *mp, struct slsfs_device **slsfsdev)
{
	struct slsfs_device *sdev;
	void *vdata;
	int error;

	/*
	 * Get a pointer to the device vnode's current private data.
	 * We need to restore it when destroying the SLOS. The field
	 * is overwritten by slsfs_create_slos.
	 */
	vdata = devvp->v_data;

	/* Create the in-memory SLOS. */
	error = slsfs_create_slos(mp, devvp);
	if (error != 0) {
		DEBUG1("slsfs_create_slos: error %d", error);
		return (error);
	}

	/*
	 * XXX Is the slsfs_device abstraction future-proofing for when we have
	 * an N to M corresponding between SLOSes and devices? Why isn't the
	 * information in the SLOS enough?
	 */
	sdev = malloc(sizeof(struct slsfs_device), M_SLSFS, M_WAITOK | M_ZERO);
	sdev->refcnt = 1;
	sdev->devvp = slos.slos_vp;
	sdev->gprovider = slos.slos_pp;
	sdev->gconsumer = slos.slos_cp;
	sdev->devsize = slos.slos_pp->mediasize;
	sdev->devblocksize = slos.slos_pp->sectorsize;
	sdev->vdata = vdata;
	mtx_init(&sdev->g_mtx, "slsfsmtx", NULL, MTX_DEF);

	DEBUG("Mounting Device Done");
	*slsfsdev = sdev;

	return (0);
}
/*
 * Vnode allocation
 * Will create the underlying inode as well and link the data to a newly
 * allocated vnode.  Does not link the allocated vnode to its parent. The
 * parent is passed in as a structure that gets us the mount point as well as
 * the slos.
 *
 * The mount point is needed to allow us to attach the vnode to it, and slos is
 * needed for access the device.
 */
static int
slsfs_valloc(
    struct vnode *dvp, mode_t mode, struct ucred *creds, struct vnode **vpp)
{
	int error;
	uint64_t pid = 0;
	struct vnode *vp;

	/* Create the new inode in the filesystem. */
	error = slos_svpalloc(VPSLOS(dvp), mode, &pid);
	if (error) {
		return (error);
	}

	/* Get a vnode for the newly created inode. */
	error = slsfs_vget(dvp->v_mount, pid, LK_EXCLUSIVE, &vp);
	if (error) {
		return (error);
	}

	/* Inherit group id from parent directory */
	SLSVP(vp)->sn_ino.ino_gid = SLSVP(dvp)->sn_ino.ino_gid;
	if (creds != NULL) {
		SLSVP(vp)->sn_ino.ino_uid = creds->cr_uid;
	}
	DEBUG2("Creating file with gid(%lu) uid(%lu)",
	    SLSVP(vp)->sn_ino.ino_gid, SLSVP(vp)->sn_ino.ino_uid);

	*vpp = vp;

	return (0);
}

/*
 * Mount the filesystem in the device backing the vnode to the mountpoint.
 */
static int
slsfs_mountfs(struct vnode *devvp, struct mount *mp)
{
	struct slsfsmount *smp = NULL;
	struct slsfs_device *slsfsdev = NULL;
	int error, ronly;

	if (mp->mnt_data == NULL) {
		ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

		/* Configure the filesystem to have the IO parameters of the
		 * device. */
		if (devvp->v_rdev->si_iosize_max != 0)
			mp->mnt_iosize_max = devvp->v_rdev->si_iosize_max;

		if (mp->mnt_iosize_max > MAXPHYS)
			mp->mnt_iosize_max = MAXPHYS;

		vfs_getopts(mp->mnt_optnew, "from", &error);
		if (error)
			goto error;

		/* Create the in-memory data for the filesystem instance. */
		smp = (struct slsfsmount *)malloc(
		    sizeof(struct slsfsmount), M_SLSFS, M_WAITOK | M_ZERO);

		smp->sp_index = -1;
		smp->sp_vfs_mount = mp;
		smp->sp_ronly = ronly;
		smp->sls_valloc = &slsfs_valloc;
		smp->sp_slos = &slos;
		mp->mnt_data = smp;

		DEBUG1("slsfs_mountfs(%p)", devvp);
		/* Create the in-memory data for the backing device. */
		DEBUG("Not a snap remount - mount device");
		error = slsfs_mount_device(devvp, mp, &slsfsdev);
		if (error) {
			free(smp, M_SLSFS);
			return error;
		}

		smp->sp_sdev = slsfsdev;
	} else {
		smp = (struct slsfsmount *)mp->mnt_data;
		error = slsfs_startupfs(mp);
		if (error)
			return (error);
	}

	KASSERT(smp->sp_slos != NULL, ("Null slos"));

	mp->mnt_data = smp;

	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_USES_BCACHE;
	MNT_IUNLOCK(mp);


	return (0);
error:
	if (smp != NULL) {
		free(smp, M_SLSFS);
		mp->mnt_data = NULL;
	}

	if (slsfsdev != NULL) {
		free(slsfsdev, M_SLSFS);
	}

	printf("Error mounting");

	return (error);
}

/*
 * Flush all dirty buffers related to the SLOS to the backend.
 *
 * Checkpointing should follow this procedure
 * 1. Sync all data - this data is marked with the current snapshot so writes to
 * the same block during the same epoch don't incur another allocation.
 *
 * 2. Mark all BTree buffers CoW - we can do this as we are managing these
 * buffers so we can mark the buffers as CoW.  So the next time we modify the
 * Btree it will make an allocation then and move the buffer over to the need
 * logical block.
 *
 * Btree's also get the advantage that since logical is physical we can cheat
 * and actually not copy the data to a new buffer. When the btrees get flushed,
 * once they get marked for CoW, we simply allocate, then move the buffer to the
 * correct logical block number within its buffer object (simply removing it
 * then inserting it with the new logical block number)
 *
 * 3. Sync all the Inodes (Syncing the SLOS_INODES_ROOT inode)
 *
 * 4. Now we have to sync the allocator btrees this, so we actually have to
 * preallocate what we will need as this may modify and dirty more data of the
 * underlying allocator btrees, we then sync the btrees with the pre-allocated
 * spaces.  We don't use the trick above as this causes a circular dependency of
 * marking a allocator node as CoW, then needed to modify the same CoW btree
 * node for a new allocation.
 *
 * There may be a way to avoid this pre allocation
 * and deal with it later but I think this would require a secondary path within
 * the btrees to know that it is the allocator and it would clear the CoW flag
 * and continue the write but then allocate a new location for itself after the
 * write is done.
 *
 * 5. Retrieve next superblock and update it with the proper information of
 * where the inodes root is, as well as the new allocation tree roots (these
 * should aways be dirty in a dirty checkpoint), update epoch and done.
 */

uint64_t checkpoints = 0;

static void
slsfs_checkpoint(struct mount *mp, int closing)
{
	struct vnode *vp, *mvp = NULL;
	struct buf *bp;
	struct slos_node *svp;
	struct slos_inode *ino;
	struct timespec te;
	diskptr_t ptr;
	int error;

again:
	/* Go through the list of vnodes attached to the filesystem. */
	MNT_VNODE_FOREACH_ACTIVE (vp, mp, mvp) {
		/* If we can't get a reference, the vnode is probably dead. */
		if (vp->v_type == VNON) {
			VI_UNLOCK(vp);
			continue;
		}

		if (vp == slos.slsfs_inodes) {
			VI_UNLOCK(vp);
			continue;
		}

		if ((error = vget(vp, LK_EXCLUSIVE | LK_INTERLOCK | LK_NOWAIT,
			 curthread)) != 0) {
			if (error == ENOENT) {
				MNT_VNODE_FOREACH_ALL_ABORT(mp, mvp);
				goto again;
			}
			continue;
		}

		// Skip over the btrees for now as we will sync them after the
		// data syncs
		if ((vp->v_vflag & VV_SYSTEM) ||
		    ((vp->v_type != VDIR) && (vp->v_type != VREG) &&
			(vp->v_type != VLNK)) ||
		    (vp->v_data == NULL)) {
			vput(vp);
			continue;
		}

		if (SLSVP(vp)->sn_status & SLOS_DIRTY) {
			/* Step 1 and 2 Sync data and mark underlying Btree Copy
			 * on write*/
			error = slos_sync_vp(vp, closing);
			if (error) {
				vput(vp);
				return;
			}

			/* Sync of data and btree complete - unmark them and
			 * update the root and dirty the root*/
			error = slos_update(SLSVP(vp));
			if (error) {
				vput(vp);
				return;
			}
		}

		vput(vp);
	}

	// Check if both the underlying Btree needs a sync or the inode itself -
	// should be a way to make it the same TODO
	// Just a hack for now to get this thing working XXX Why is it a hack?
	/* Sync the inode root itself. */
	if (slos.slos_sb->sb_data_synced) {
		error = slos_blkalloc(&slos, BLKSIZE(&slos), &ptr);
		MPASS(error == 0);

		DEBUG("Checkpointing the inodes btree");
		/* 3 Sync Root Inodes and btree */
		error = vn_lock(slos.slsfs_inodes, LK_EXCLUSIVE);
		if (error) {
			panic("vn_lock failed");
		}
		svp = SLSVP(slos.slsfs_inodes);
		ino = &svp->sn_ino;
		DEBUG2(
		    "Flushing inodes %p %p", slos.slsfs_inodes, svp->sn_fdev);
		error = slos_sync_vp(slos.slsfs_inodes, closing);
		if (error) {
			panic("slos_sync_vp failed to checkpoint");
			return;
		}

		/*
		 * Allocate a new blk for the root inode write it and give it
		 * to the superblock
		 */
		// Write out the root inode
		DEBUG("Creating the new superblock");
		ino->ino_blk = ptr.offset;

		slos.slos_sb->sb_root.offset = ino->ino_blk;

		bp = getblk(svp->sn_fdev, ptr.offset, BLKSIZE(&slos), 0, 0, 0);
		MPASS(bp);
		memcpy(bp->b_data, ino, sizeof(struct slos_inode));
		bawrite(bp);

		DEBUG("Checkpointing the checksum tree");
		// Write out the checksum tree;
		error = slos_blkalloc(&slos, BLKSIZE(&slos), &ptr);
		if (error) {
			panic("Problem with allocation");
		}

		DEBUG1("Flushing checksum %p",
		    slos.slos_cktree->sn_tree.bt_backend);
		ino = &slos.slos_cktree->sn_ino;
		MPASS(slos.slos_cktree != SLSVP(slos.slsfs_inodes));
		ino->ino_blk = ptr.offset;
		if (checksum_enabled)
			fbtree_sync(&slos.slos_cktree->sn_tree);
		bp = getblk(svp->sn_fdev, ptr.offset, BLKSIZE(&slos), 0, 0, 0);
		MPASS(bp);
		memcpy(bp->b_data, ino, sizeof(struct slos_inode));
		/* Async because we have a barrier below. */
		bawrite(bp);

		slos.slos_sb->sb_cksumtree = ptr;

		DEBUG1("Checksum tree at %lu", ptr.offset);
		DEBUG1("Root Dir at %lu",
		    SLSVP(slos.slsfs_inodes)->sn_ino.ino_blk);
		DEBUG1("Inodes File at %lu", slos.slos_sb->sb_root.offset);
		MPASS(ptr.offset != slos.slos_sb->sb_root.offset);

		slos.slos_sb->sb_index = (slos.slos_sb->sb_epoch) % 100;

		/* 4 Sync the allocator */
		DEBUG("Syncing the allocator");
		slos_allocator_sync(&slos, slos.slos_sb);
		DEBUG2("Epoch %lu done at superblock index %u",
		    slos.slos_sb->sb_epoch, slos.slos_sb->sb_index);
		SLSVP(slos.slsfs_inodes)->sn_status &= ~(SLOS_DIRTY);

		/* Flush the current superblock itself. */
		bp = getblk(slos.slos_vp, slos.slos_sb->sb_index,
		    slos.slos_sb->sb_ssize, 0, 0, 0);
		MPASS(bp);
		memcpy(bp->b_data, slos.slos_sb, sizeof(struct slos_sb));

		DEBUG("Flushing the checksum tree again");
		if (checksum_enabled)
			fbtree_sync(&slos.slos_cktree->sn_tree);

		nanotime(&te);
		slos.slos_sb->sb_time = te.tv_sec;
		slos.slos_sb->sb_time_nsec = te.tv_nsec;

		bbarrierwrite(bp);

		VOP_UNLOCK(slos.slsfs_inodes, 0);

		checkpoints++;
		DEBUG3("Checkpoint: %lu, %lu, %lu", checkpoints,
		    slos.slos_sb->sb_data_synced, slos.slos_sb->sb_meta_synced);

		slos.slos_sb->sb_data_synced = 0;
		slos.slos_sb->sb_meta_synced = 0;
		slos.slos_sb->sb_attempted_checkpoints = 0;
		slos.slos_sb->sb_epoch += 1;
	} else {
		slos.slos_sb->sb_attempted_checkpoints++;
	}
}

uint64_t checkpointtime = 100;
/*
 * Daemon that flushes dirty buffers to the device.
 *
 * XXX It's very interesting how this combines with
 * explicit checkpoints
 */
static void
slsfs_syncer(struct slos *slos)
{
	slos->slsfs_sync_exit = 0;
	struct timespec ts, te;
	uint64_t elapsed, period;

	/* Periodically sync until we unmount. */
	mtx_lock(&slos->slsfs_sync_lk);
	while (!slos->slsfs_sync_exit) {
		slos->slsfs_syncing = 1;
		mtx_unlock(&slos->slsfs_sync_lk);
		nanotime(&ts);
		slsfs_checkpoint(slos->slsfs_mount, 0);
		nanotime(&te);
		/* Notify anyone waiting to synchronize. */
		mtx_lock(&slos->slsfs_sync_lk);
		slos->slsfs_syncing = 0;
		cv_broadcast(&slos->slsfs_sync_cv);
		mtx_unlock(&slos->slsfs_sync_lk);

		elapsed = (1000000000ULL * (te.tv_sec - ts.tv_sec)) +
		    (te.tv_nsec - ts.tv_nsec);

		period = checkpointtime * 1000000ULL;

		te.tv_sec = 0;
		if (elapsed < period) {
			te.tv_nsec = period - elapsed;
		} else {
			te.tv_nsec = 0;
		}

		/* Wait until it's time to flush again. */
		mtx_lock(&slos->slsfs_sync_lk);
		if (te.tv_nsec > 0) {
			msleep_sbt(&slos->slsfs_syncing, &slos->slsfs_sync_lk,
			    PRIBIO, "Sync-wait", SBT_1NS * te.tv_nsec, 0,
			    C_HARDCLOCK);
		}
	}

	DEBUG("Syncer exiting");
	slos->slsfs_syncing = 1;
	mtx_unlock(&slos->slsfs_sync_lk);
	/* One last checkpoint before we exit. */
	slsfs_checkpoint(slos->slsfs_mount, 1);

	mtx_lock(&slos->slsfs_sync_lk);
	/* Notify anyone else waiting to flush one last time. */
	slos->slsfs_syncing = 0;
	DEBUG("Wake- up external");
	cv_broadcast(&slos->slsfs_sync_cv);

	DEBUG("Syncer exited");
	slos->slsfs_syncertd = NULL;
	mtx_unlock(&slos->slsfs_sync_lk);
	kthread_exit();
}

/*
 * Initialize the in-memory state of the filesystem instance.
 */
static int
slsfs_init_fs(struct mount *mp)
{
	struct vnode *vp = NULL;
	int error;

	/* Get the filesystem root and initialize it if this is the first mount.
	 */
	VFS_ROOT(mp, LK_EXCLUSIVE, &vp);
	if (vp == NULL) {
		return (EIO);
	}

	/* Set up the syncer. */
	slos.slsfs_mount = mp;
	error = kthread_add((void (*)(void *))slsfs_syncer, &slos, NULL,
	    &slos.slsfs_syncertd, 0, 0, "slsfs syncer");
	if (error) {
		panic("Syncer could not start");
	}

	/* Initialize the root directory on first mount */
	if (SLSINO(SLSVP(vp)).ino_nlink < 2) {
		slsfs_init_dir(vp, vp, NULL);
	}

	vput(vp);

	return (0);
}

/*
 * Wake up the SLOS syncer.
 */
int
slsfs_wakeup_syncer(int is_exiting)
{
	/* Don't sync again if already in progress. */
	/* XXX Maybe exit if it's already in progress? How do we
	 * serialize writes and syncs? (If a write after the last
	 * sync but before this one doesn't get  written by the former
	 * then we have to go through with the latter).
	 */

	mtx_lock(&slos.slsfs_sync_lk);
	if (slos.slsfs_syncertd == NULL) {
		mtx_unlock(&slos.slsfs_sync_lk);
		return (0);
	}

	if (slos.slsfs_syncing) {
		cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	}

	slos.slsfs_syncing = 1;
	if (is_exiting) {
		slos.slsfs_sync_exit = 1;
	}

	/* The actual wakeup. */
	wakeup(&slos.slsfs_syncing);

	if (slos.slsfs_syncertd == NULL) {
		mtx_unlock(&slos.slsfs_sync_lk);
		return (0);
	}

	/* Wait until the syncer notifies us it's done. */
	while (slos.slsfs_syncing)
		cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	mtx_unlock(&slos.slsfs_sync_lk);

	return (0);
}

static int
slsfs_free_system_vnode(struct vnode *vp)
{
	struct slos_node *svp;
	svp = SLSVP(vp);
	vrele(vp);

	VOP_LOCK(vp, LK_EXCLUSIVE);
	VOP_RECLAIM(vp, curthread);
	VOP_UNLOCK(vp, 0);

	return 0;
}

/*
 * Mount the filesystem.
 */
static int
slsfs_mount(struct mount *mp)
{
	DEBUG("Mounting slsfs");
	struct vnode *devvp = NULL;
	struct nameidata nd;
	struct vfsoptlist *opts;
	int error = 0;
	enum slos_state oldstate;
	char *from;

	DEBUG("Mounting drive");

	/* We do nothing on updates. */
	if (mp->mnt_flag & MNT_UPDATE)
		return (0);

	SLOS_LOCK(&slos);
	/* Cannot mount twice. */
	if ((slos_getstate(&slos) != SLOS_UNMOUNTED) &&
	    (slos_getstate(&slos) != SLOS_SNAPCHANGE)) {
		SLOS_UNLOCK(&slos);
		return (EBUSY);
	}

	oldstate = slos_getstate(&slos);
	if (oldstate == SLOS_SNAPCHANGE) {
		KASSERT(mp->mnt_data != NULL,
		    ("Requires mount data for snapshot remount"));
		struct slsfsmount *smp = mp->mnt_data;
		// Argument of index -1 means we want the latest snapshot and
		// we can write at that point
		if (smp->sp_index != -1) {
			mp->mnt_flag |= MNT_RDONLY;
		} else {
			mp->mnt_flag &= ~(MNT_RDONLY);
		}
	}

	slos_setstate(&slos, SLOS_INFLUX);
	SLOS_UNLOCK(&slos);

	if (mp->mnt_data != NULL) {
		slsfs_wakeup_syncer(1);
		vflush(mp, 0, FORCECLOSE, curthread);

		slsfs_free_system_vnode(slos.slsfs_inodes);
		slos.slsfs_inodes = NULL;

		VOP_LOCK(slos.slos_vp, LK_EXCLUSIVE);

		error = slsfs_mountfs(slos.slos_vp, mp);
		if (error != 0) {
			VOP_UNLOCK(slos.slos_vp, 0);
			goto error;
		}

		error = slsfs_init_fs(mp);
		if (error != 0) {
			VOP_UNLOCK(slos.slos_vp, 0);
			goto error;
		}

		VOP_UNLOCK(slos.slos_vp, 0);
	} else {
		opts = mp->mnt_optnew;
		vfs_filteropt(opts, slsfs_opts);

		from = vfs_getopts(opts, "from", &error);
		if (error != 0)
			goto error;

		NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from,
		    curthread);
		error = namei(&nd);
		if (error)
			goto error;
		NDFREE(&nd, NDF_ONLY_PNBUF);

		devvp = nd.ni_vp;
		if (!vn_isdisk(devvp, &error)) {
			/* XXX Can we make it so we can use a file? */
			DEBUG("Is not a disk");
			vput(devvp);
			goto error;
		}

		/* Get an ID for the new filesystem. */
		vfs_getnewfsid(mp);

		/* Mount the filesystem and initialize its state. */
		error = slsfs_mountfs(devvp, mp);
		if (error) {
			vput(devvp);
			goto error;
		}

		VOP_UNLOCK(slos.slos_vp, 0);

		error = slsfs_init_fs(mp);
		if (error) {
			/* XXX Cleanup init'ed state. */
			printf("Couldn't init\n");
			vrele(devvp);
			goto error;
		}

		/* Get the path where we found the device. */
		vfs_mountedfrom(mp, from);
	}

	/* Remove the SLOS from the flux state. */
	SLOS_LOCK(&slos);
	slos_setstate(&slos, SLOS_MOUNTED);
	SLOS_UNLOCK(&slos);

	return (0);

error:

	/* Remove the SLOS from the flux state. */
	SLOS_LOCK(&slos);
	slos_setstate(&slos, oldstate);
	SLOS_UNLOCK(&slos);

	return (error);
}

/*
 *  Return the vnode for the root of the filesystem.
 */
static int
slsfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	/*
	 * Use the already implemented VFS method with the
	 * hardcoded value for the filesystem root.
	 */
	error = VFS_VGET(mp, SLOS_ROOT_INODE, flags, &vp);
	if (error)
		return (error);

	vp->v_type = VDIR;
	*vpp = vp;

	return (0);
}

/*
 * Export stats for the filesystem.
 * XXX Implement eventually
 */
static int
slsfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct slsfs_device *slsdev;
	struct slsfsmount *smp;
	struct slos_sb *sb = slos.slos_sb;

	smp = TOSMP(mp);
	slsdev = smp->sp_sdev;
	sbp->f_bsize = sb->sb_bsize;
	sbp->f_iosize = sb->sb_bsize;
	sbp->f_blocks = sb->sb_size;
	/* XXX We are not accounting for metadata blocks (e.g. trees). */
	sbp->f_bfree = sbp->f_bavail = sb->sb_size - sb->sb_used;
	/* We have no limit on the amount of inodes. */
	sbp->f_files = sbp->f_ffree = UINT_MAX;
	sbp->f_namemax = SLSFS_NAME_LEN;

	return (0);
}

/*
 * Unmount a device from the SLOS system and the kernel.
 */
static int
slsfs_unmount_device(struct slsfs_device *sdev)
{
	int error = 0;

	SLOS_LOCK(&slos);

	/* Unhook the SLOS from the GEOM layer. */
	if (slos.slos_cp != NULL) {
		g_topology_lock();
		g_vfs_close(slos.slos_cp);
		dev_ref(slos.slos_vp->v_rdev);
		g_topology_unlock();
	}

	SLOS_UNLOCK(&slos);

	slos_allocator_uninit(&slos);

	free(slos.slos_sb, M_SLOS_SB);

	/* Destroy the device. */
	mtx_destroy(&sdev->g_mtx);

	/* Restore the node's private data. */
	sdev->devvp->v_data = sdev->vdata;

	free(sdev, M_SLSFS);

	DEBUG("Device Unmounted");

	return error;
}

/*
 * Destroy the mounted filesystem data.
 */
static void
slsfs_freemntinfo(struct mount *mp)
{
	DEBUG("Destroying slsmount info");
	struct slsfsmount *smp = TOSMP(mp);

	if (mp == NULL)
		return;

	free(smp, M_SLSFS);
	DEBUG("Destroyed slsmount info");
}

/*
 * Unmount the filesystem from the kernel.
 */
static int
slsfs_unmount(struct mount *mp, int mntflags)
{
	struct slsfs_device *sdev;
	struct slsfsmount *smp;
	struct slos *slos;
	int error;
	int flags = 0;

	smp = mp->mnt_data;
	sdev = smp->sp_sdev;
	slos = smp->sp_slos;

	KASSERT(slos->slsfs_mount != NULL, ("no mount"));

	SLOS_LOCK(slos);
	if (slos_getstate(slos) != SLOS_MOUNTED &&
	    ((mntflags & MNT_FORCE) == 0)) {
		KASSERT(slos_getstate(slos) != SLOS_UNMOUNTED,
		    ("unmounting nonexistent slsfs"));
		SLOS_UNLOCK(slos);
		return (EBUSY);
	}

	slos_setstate(slos, SLOS_INFLUX);
	SLOS_UNLOCK(slos);

	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
	}

	/* Free the slos taskqueue, if we have not already done so. */
	/*
	 * XXX Can we move this to after we ensure this function succeeds?
	 * If we fail to flush we are stuck with a file system with a
	 * nonexistent taskqueue. If the SLS reattaches and tries to reuse it we
	 * will crash.
	 */
	if (slos->slos_tq != NULL)
		taskqueue_free(slos->slos_tq);
	slos->slos_tq = NULL;

	/*
	 * Flush the data to the disk. We have already removed all
	 * vnodes, so this is going to be the last flush we need.
	 */
	slsfs_wakeup_syncer(1);

	error = vflush(mp, 0, flags, curthread);
	if (error) {
		printf("vflush failed with %d\n", error);
		goto error;
	}

	/*
	 * Manually destroy the root inode. This works as long as everything
	 * has been flushed above.
	 */
	slsfs_free_system_vnode(slos->slsfs_inodes);
	slos->slsfs_inodes = NULL;

	// Free the checksum tree
	slos_vpfree(slos, slos->slos_cktree);
	slos->slos_cktree = NULL;

	DEBUG("Flushed all active vnodes");
	/* Remove the mounted device. */
	error = slsfs_unmount_device(sdev);
	if (error)
		goto error;

	cv_destroy(&slos->slsfs_sync_cv);
	mtx_destroy(&slos->slsfs_sync_lk);
	slsfs_freemntinfo(mp);

	DEBUG("Freeing mount info");
	mp->mnt_data = NULL;
	slos->slsfs_mount = NULL;
	DEBUG("Changing mount flags");

	/* We've removed the local filesystem info. */
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	/* Couldn't unmount after all. */
	SLOS_LOCK(slos);
	slos_setstate(slos, SLOS_UNMOUNTED);
	SLOS_UNLOCK(slos);

	return (0);

error:
	/* Couldn't unmount after all. */
	SLOS_LOCK(slos);
	slos_setstate(slos, SLOS_MOUNTED);
	SLOS_UNLOCK(slos);

	printf("WARNING: SLOS failed unmount (error %d)\n", error);
	return (error);
}

static void
slsfs_init_vnode(struct vnode *vp, uint64_t ino)
{
	struct slos_node *mp = SLSVP(vp);

	switch (ino) {
	case SLOS_ROOT_INODE:
		vp->v_vflag |= VV_ROOT;
		vp->v_type = VDIR;
		SLSVP(vp)->sn_ino.ino_gid = 0;
		SLSVP(vp)->sn_ino.ino_uid = 0;
		break;
	case SLOS_INODES_ROOT:
		vp->v_type = VREG;
		vp->v_vflag |= VV_SYSTEM;
		break;
	case SLOS_SLSPART_INODE:
	case SLOS_SLSPREFAULT_INODE:
		vp->v_type = VREG;
		break;
	default:
		vp->v_type = IFTOVT(mp->sn_ino.ino_mode);
	}
	SLSVP(vp)->sn_ino.ino_wal_segment.size = 0;
	SLSVP(vp)->sn_ino.ino_wal_segment.offset = 0;

	if (vp->v_type == VFIFO) {
		vp->v_op = &slsfs_fifoops;
	}

	vnode_create_vobject(vp, 0, curthread);
}

/*
 * Get a new vnode for the specified SLOS inode.
 */
static int
slsfs_vget(struct mount *mp, uint64_t ino, int flags, struct vnode **vpp)
{
	int error;
	struct vnode *vp;
	struct vnode *none;
	struct slos_node *svnode;
	struct thread *td;

	td = curthread;
	vp = NULL;
	none = NULL;

	ino = OIDTOSLSID(ino);

	/* Make sure the inode does not already have a vnode. */
	error = vfs_hash_get(mp, ino, LK_EXCLUSIVE, td, &vp, NULL, NULL);
	if (error) {
		return (error);
	}

	/* If we do have a vnode already, return it. */
	if (vp != NULL) {
		*vpp = vp;
		return (0);
	}

	/* Bring the inode in memory. */
	error = slos_iopen(&slos, ino, &svnode);
	if (error) {
		*vpp = NULL;
		return (error);
	}

	/* Get a new blank vnode. */
	error = getnewvnode("slsfs", mp, &slsfs_vnodeops, &vp);
	if (error) {
		DEBUG("Problem getting new inode");
		*vpp = NULL;
		return (error);
	}

	/*
	 * If the vnode is not the root, which is managed directly
	 * by the SLOS, add it to the mountpoint.
	 */
	vn_lock(vp, LK_EXCLUSIVE);
	if (ino != SLOS_INODES_ROOT) {
		error = insmntque(vp, mp);
		if (error) {
			DEBUG("Problem queing root into mount point");
			*vpp = NULL;
			return (error);
		}
	}

	svnode->sn_slos = &slos;
	vp->v_data = svnode;
	vp->v_bufobj.bo_ops = &bufops_slsfs;
	vp->v_bufobj.bo_bsize = IOSIZE(svnode);
	slsfs_init_vnode(vp, ino);

	/* Again, if we're not the inode metanode, add bookkeeping. */
	/*
	 * XXX Why? This means we would get a different vnode every time
	 * we try to open the metanode.
	 */
	*vpp = NULL;
	if (ino != SLOS_INODES_ROOT) {
		/*
		 * Try to insert the new node into the table. We might have been
		 * beaten to it by another process, in which case we reuse their
		 * fresh vnode for the inode.
		 */
		error = vfs_hash_insert(
		    vp, ino, LK_EXCLUSIVE, td, vpp, NULL, NULL);
		if (error != 0) {
			*vpp = NULL;
			return (error);
		}
	}
	DEBUG2("vget(%p) ino = %ld", vp, ino);
	/* If we weren't beaten to it, propagate the new node to the caller. */
	if (*vpp == NULL)
		*vpp = vp;

	return (0);
}

static int
slsfs_sync(struct mount *mp, int waitfor)
{
	return (0);
}

static struct vfsops slsfs_vfsops = { .vfs_init = slsfs_init,
	.vfs_uninit = slsfs_uninit,
	.vfs_root = slsfs_root,
	.vfs_statfs = slsfs_statfs,
	.vfs_mount = slsfs_mount,
	.vfs_unmount = slsfs_unmount,
	.vfs_vget = slsfs_vget,
	.vfs_sync = slsfs_sync };

VFS_SET(slsfs_vfsops, slsfs, 0);
MODULE_VERSION(slsfs, 0);
