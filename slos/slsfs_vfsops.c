#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/unistd.h>
#include <sys/extattr.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lockf.h>
#include <sys/module.h>
#include <sys/namei.h>
#include <sys/uio.h>
#include <sys/priv.h>
#include <sys/conf.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/kthread.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#include <slos.h>
#include <slos_record.h>
#include <slos_inode.h>
#include <slos_btree.h>
#include <slos_io.h>
#include <slosmm.h>
#include <btree.h>

#include "slos_alloc.h"
#include "slsfs_alloc.h"

#include "slsfs_subr.h"
#include "slsfs.h"
#include "slsfs_dir.h"
#include "slsfs_buf.h"
#include "slsfs_alloc.h"


static MALLOC_DEFINE(M_SLSFSMNT, "slsfs_mount", "SLS mount structures");

static vfs_root_t	slsfs_root;
static vfs_statfs_t	slsfs_statfs;
static vfs_vget_t	slsfs_vget;
static vfs_sync_t	slsfs_sync;
extern struct buf_ops bufops_slsfs;

static const char *slsfs_opts[] = { "from" };
struct unrhdr *slsid_unr;

/*
 * Register the Aurora filesystem type with the kernel.
 */
static int
slsfs_init(struct vfsconf *vfsp)
{
        /* Get a new unique identifier generator. */
        slsid_unr = new_unrhdr(0, INT_MAX, NULL);
	fnode_zone = uma_zcreate("Btree Fnode slabs", sizeof(struct fnode), NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);

        /* The constructor never fails. */
        KASSERT(slsid_unr != NULL, ("slsid unr creation failed"));;
	return (0);
}

/*
 * Unregister the Aurora filesystem type from the kernel.
 */
static int
slsfs_uninit(struct vfsconf *vfsp)
{
        /* Destroy the identifier generator. */
	clean_unrhdr(slsid_unr);
	clear_unrhdr(slsid_unr);
        delete_unrhdr(slsid_unr);
        slsid_unr = NULL;
	uma_zdestroy(fnode_zone);

	return (0);
}

/*
 * Bring the inode that holds the rest of the filesystem inodes into memory.
 */
static int
slsfs_inodes_init(struct mount *mp, struct slos *slos)
{
	struct slos_node *node;
	int error;

        /* Create the vnode for the inode root. */
	error = slsfs_vget(mp, SLOS_INODES_ROOT, 0, &slos->slsfs_inodes);
	if (error) {
		return (error);
	}

	node = SLSVP(slos->slsfs_inodes);

        /* Create the filesystem root. */
	DBUG("Initing the root inode\n");
	error = slos_icreate(slos, SLOS_ROOT_INODE, MAKEIMODE(VDIR, S_IRWXU));
	if (error == EINVAL) {
		DBUG("Already exists\n");
	} else if (error) {
		return (error);
	}

	return (0);
}

/*
 * Create an in-memory representation of the SLOS.
 */
static int
slsfs_create_slos(struct mount *mp, struct vnode *devvp)
{
	int error;


	slos.slos_vp = devvp;

        /* Hook up the SLOS into the GEOM provider for the backing device. */
	g_topology_lock();
	error = g_vfs_open(devvp, &slos.slos_cp, "slsfs", 1);
	if (error) {
		printf("Error in opening GEOM vfs\n");
		return error;
	}
	slos.slos_pp = g_dev_getprovider(devvp->v_rdev);
	g_topology_unlock();

	/* Read in the superblock. */
	DBUG("SLOS Read in Super\n");
	error = slos_sbread(&slos);
	if (error != 0) {
		DBUG("ERROR: slos_sbread failed with %d\n", error);
		printf("Problem with slos\n");
		if (slos.slos_cp != NULL) {
			g_topology_lock();
			g_vfs_close(slos.slos_cp);
			dev_ref(slos.slos_vp->v_rdev);
			g_topology_unlock();
		}
		return error;
	}

	lockinit(&slos.slos_lock, PVFS, "sloslock", VLKTIMEOUT, LK_NOSHARE);
	cv_init(&slos.slsfs_sync_cv, "SLSFS Syncer CV");
	mtx_init(&slos.slsfs_sync_lk, "syncer lock",  NULL, MTX_DEF);
	slos.slsfs_dirtybufcnt = 0;
	slos.slsfs_syncing = 0;
	slos.slsfs_checkpointtime = 1;
	slos.slsfs_mount = mp;
	devvp->v_data = &slos;

	// This is all quite hacky just to get it working though
        /* Get a vnode for the device. XXX Is this for direct IOs? */
	error = getnewvnode("SLSFS Fake VNode", mp, &sls_vnodeops, &slos.slsfs_dev);
	if (error) {
		printf("Problem getting fake vnode for device\n");
		return (error);
        }

        /* Set up the necessary backend state to be able to do IOs to the device. */
	DBUG("Blocksize %lu\n", slos.slos_sb->sb_bsize);
	slos.slsfs_dev->v_bufobj.bo_ops = &bufops_slsfs;
	slos.slsfs_dev->v_bufobj.bo_bsize = slos.slos_sb->sb_bsize;
	DBUG("OldType %x\n", slos.slsfs_dev->v_type);
	slos.slsfs_dev->v_type = VCHR;
	slos.slsfs_dev->v_data = &slos;
	slos.slsfs_dev->v_vflag |= VV_SYSTEM;

	DBUG("Setup Fake Device\n");
        /* Initialize in memory the allocator and the vnode used for inode bookkeeping. */
	slsfs_allocator_init(&slos);
	slsfs_inodes_init(mp, &slos);
	VOP_UNLOCK(slos.slos_vp, 0);

	DBUG("SLOS Loaded.\n");
	return error;
}

/*
 * Mount a device as a SLOS, and create an in-memory representation.
 */
static int
slsfs_mount_device(struct vnode *devvp, struct mount *mp, struct slsfs_device **slsfsdev)
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
		printf("Error creating SLOS - %d\n", error);
		return (error);
	}


        /*
         * XXX Is the slsfs_device abstraction future-proofing for when we have an
         * N to M corresponding between SLOSes and devices? Why isn't the information
         * in the SLOS enough?
         */
	sdev = malloc(sizeof(struct slsfs_device), M_SLSFSMNT, M_WAITOK | M_ZERO);
	sdev->refcnt = 1;
	sdev->devvp = slos.slos_vp;
	sdev->gprovider = slos.slos_pp;
	sdev->gconsumer = slos.slos_cp;
	sdev->devsize = slos.slos_pp->mediasize;
	sdev->devblocksize = slos.slos_pp->sectorsize;
	sdev->vdata = vdata;
	mtx_init(&sdev->g_mtx, "slsfsmtx", NULL, MTX_DEF);

	DBUG("Mounting Device Done\n");
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
slsfs_valloc(struct vnode *dvp, mode_t mode, struct ucred *creds, struct vnode **vpp)
{
	int error;
	uint64_t pid = 0;

        /* Create the new inode in the filesystem. */
	error = slsfs_new_node(VPSLOS(dvp), mode, &pid);
	if (error) {
		return (error);
	}

        /* Get a vnode for the newly created inode. */
	error = slsfs_vget(dvp->v_mount, pid, LK_EXCLUSIVE, vpp);
	if (error) {
		return (error);
	}

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
	char * from;

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

        /* Configure the filesystem to have the IO parameters of the device. */
	if (devvp->v_rdev->si_iosize_max != 0)
		mp->mnt_iosize_max = devvp->v_rdev->si_iosize_max;

	if (mp->mnt_iosize_max > MAXPHYS)
		mp->mnt_iosize_max = MAXPHYS;

	from = vfs_getopts(mp->mnt_optnew, "from", &error);
	if (error)
		goto error;

        /* Create the in-memory data for the filesystem instance. */
	smp = (struct slsfsmount *) malloc(sizeof(struct slsfsmount), M_SLSFSMNT, M_WAITOK | M_ZERO);
	if (error) {
		goto error;
	}

	smp->sp_vfs_mount = mp;
	smp->sp_ronly = ronly;
	smp->sls_valloc = &slsfs_valloc;
	smp->sp_slos = &slos;

	CTR1(KTR_SPARE5, "slsfs_mountfs(%p)", devvp);
        /* Create the in-memory data for the backing device. */
	error = slsfs_mount_device(devvp, mp, &slsfsdev);
	if (error) {
		goto error;
	}

	smp->sp_sdev = slsfsdev;

	KASSERT(smp->sp_slos != NULL, ("Null slos"));
	if (error)
		goto error;

	mp->mnt_data = smp;

	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_USES_BCACHE;
	MNT_IUNLOCK(mp);

	return (0);
error:
	if (smp != NULL) {
		free(smp, M_SLSFSMNT);
		mp->mnt_data = NULL;
	}

	if (slsfsdev != NULL) {
		free(slsfsdev, M_SLSFSMNT);
	}

	printf("Error mounting");
	vput(devvp);

	return (error);
}

/*
 * Flush all dirty buffers related to the SLOS to the backend.
 */
static void
slsfs_checkpoint(struct mount *mp)
{
	struct vnode *vp, *mvp;
	struct thread *td;
	struct bufobj *bo;
	int error;

	td = curthread;
	/* Go throught the list of vnodes attached to the filesystem. */
	MNT_VNODE_FOREACH_ACTIVE(vp, mp, mvp) {
                /* XXX Is this right? Why are we unlocking the vnode's VI lock? We never locked it.  */
		if(VOP_ISLOCKED(vp)) {
			VI_UNLOCK(vp);
			continue;
		}

                /* If we can't get a reference, the vnode is probably dead. */
		if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK | LK_NOWAIT, td) != 0) {
			continue;
		}

		bo = &vp->v_bufobj;
		// XXX We cant let the slsfs_dev sync normally as its a fake inode
		// and doesnt have all the interconects a regular one does. We
		// may one to flush it out and just make it easier for the
		// abstraction, meaning make a fake inode and fake slos_node
		// for it as well.
                /* Only sync the vnode if it has been dirtied. */
		if (bo->bo_dirty.bv_cnt) {
			/* XXX Why return on an error? We should keep going 
			 * with the rest. */
			error = slsfs_sync_vp(vp);
			if (error) {
				vput(vp);
				return;
			}

                        /* Update the inode root with the inode's new information. */
			error = slos_updateroot(SLSVP(vp));
			if (error) {
				vput(vp);
				return;
			}
		}
		vput(vp);
	}

	// Just a hack for now to get this thing working XXX Why is it a hack?
        /* Sync the inode root itself. */
	VOP_LOCK(slos.slsfs_inodes, LK_EXCLUSIVE);
	error = slsfs_sync_vp(slos.slsfs_inodes);
	if (error) {
		return;
	}
	VOP_UNLOCK(slos.slsfs_inodes, 0);

	// Just a hack for now to get this thing working XXX Same
        /* Sync any raw device buffers. */
	VOP_LOCK(slos.slsfs_dev, LK_EXCLUSIVE);
	error = slsfs_sync_dev(&slos);
	if (error) {
		VOP_UNLOCK(slos.slsfs_dev, 0);
		return;
	}
	VOP_UNLOCK(slos.slsfs_dev, 0);
}

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

        /* Periodically sync until we unmount. */
	while (!slos->slsfs_sync_exit) {
		slsfs_checkpoint(slos->slsfs_mount);

                /* Notify anyone waiting to synchronize. */
		mtx_lock(&slos->slsfs_sync_lk);
		slos->slsfs_syncing = 0;
		cv_broadcast(&slos->slsfs_sync_cv);
		mtx_unlock(&slos->slsfs_sync_lk);

                /* Wait until it's time to flush again. */
		if (!slos->slsfs_sync_exit) {
		    tsleep(&slos->slsfs_syncing, PRIBIO, "-",
			    (hz * slos->slsfs_checkpointtime) >> 1);
		}
	}

	DBUG("Syncer exiting\n");

        /* One last checkpoint before we exit. */
	slsfs_checkpoint(slos->slsfs_mount);

        /* Notify anyone else waiting to flush one last time. */
	mtx_lock(&slos->slsfs_sync_lk);
	slos->slsfs_syncing = 0;
	cv_broadcast(&slos->slsfs_sync_cv);
	mtx_unlock(&slos->slsfs_sync_lk);

	slos->slsfs_syncertd = NULL;

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

        /* Get the filesystem root and initialize it if this is the first mount. */
	VFS_ROOT(mp, LK_EXCLUSIVE, &vp);
	if (vp == NULL) {
		return (EIO);
	}

	/* Set up the syncer. */
	slos.slsfs_mount = mp;
	error = kthread_add((void(*)(void *))slsfs_syncer, &slos, NULL,
		&slos.slsfs_syncertd, 0, 0, "slsfs syncer");
	if (error) {
		panic("Syncer could not start");
	}

	/*
	 * I have no idea when this bug occured but right now ls -la doesnt 
	 * show up the first directory that gets created.  Even though with 
	 * debugging on I can see it within the buffer and it shows correctly 
	 * there.
	 *
	 * This is a hack as if i create and remove a directory it works 
	 * perfectly fine.
	 *
	 * XXX: Fix this bug
	 *
	 */
	if (SLSINO(SLSVP(vp)).ino_nlink < 2) {
		struct componentname name;
		struct vattr attr = {};
		char * buf = "hackme";
		name.cn_nameptr = buf;
		name.cn_namelen = 5;
		slsfs_init_dir(vp, vp, NULL);
		struct vnode *svp;
		VOP_MKDIR(vp, &svp, &name, &attr);
		VOP_RMDIR(vp, svp, &name);
		VOP_CLOSE(svp, 0, NULL, NULL);
		vput(svp);
	}

	vput(vp);

	return (0);
}

/*
 * Mount the filesystem.
 */
static int
slsfs_mount(struct mount *mp)
{
	DBUG("Mounting slsfs\n");
	struct vnode *devvp;
	struct nameidata nd;
	struct vfsoptlist *opts;
	int error = 0;
	char *from;

	opts = mp->mnt_optnew;
	vfs_filteropt(opts, slsfs_opts);
	if (mp->mnt_flag & MNT_UPDATE) {
		// TODO: Update of Mount
	}

        /* Get the vnode device to mount from the path. */
	from = vfs_getopts(opts, "from", &error);
	if (error != 0) {
		return (error);
	}

	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from, curthread);
	error = namei(&nd);
	if (error) {
		return (error);
	}
	NDFREE(&nd, NDF_ONLY_PNBUF);

	devvp = nd.ni_vp;
	if (!vn_isdisk(devvp, &error)) {
                /* XXX Can we make it so we can use a file? */
		DBUG("Dbug is not a disk\n");
		vput(devvp);
		return (error);
	}

        /* XXX This seems like it's a leftover from before.  */
	if (error) {
		vput(devvp);
		return (error);
	}

        /* Get an ID for the new filesystem. */
	vfs_getnewfsid(mp);

        /* Actually mount the vnode as a filesyste and initialize its state. */
	printf("Mounting fs\n");
	error = slsfs_mountfs(devvp, mp);
	if (error) {
		return (error);
	}

	error = slsfs_init_fs(mp);
	if (error) {
		return (error);
	}

        /* Get the path where we found the device. */
	vfs_mountedfrom(mp, from);

	return (0);
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
	return(0);
}

/*
 * Export stats for the filesystem.
 * XXX Implement eventually
 */
static int
slsfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct slsfs_device	    *slsdev;
	struct slsfsmount	    *smp;

	smp = TOSMP(mp);
	slsdev = smp->sp_sdev;
	sbp->f_bsize = slsdev->devblocksize;
	sbp->f_iosize = sbp->f_bsize;
	sbp->f_blocks = 1000;
	sbp->f_bfree = 100;
	sbp->f_bavail = 100;
	sbp->f_ffree = 0;
	sbp->f_files = 1000;

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

        /* Destroy related in-memory locks. */
	lockdestroy(&slos.slos_lock);
	vnode_destroy_vobject(slos.slsfs_dev);

	slos.slsfs_dev = NULL;
	free(slos.slos_sb, M_SLOS);

        /* Destroy the device. */
	mtx_destroy(&sdev->g_mtx);

	/* Restore the node's private data. */
	sdev->devvp->v_data = sdev->vdata;

	free(sdev, M_SLSFSMNT);

	DBUG("Device Unmounted\n");

	return error;
}

/*
 * Destroy the mounted filesystem data.
 */
static void
slsfs_freemntinfo(struct mount *mp)
{
	DBUG("Destroying slsmount info\n");
	struct slsfsmount *smp = TOSMP(mp);

	if (mp == NULL)
		return;

	free(smp, M_SLSFSMNT);
	DBUG("Destroyed slsmount info\n");
}

/*
 * Wake up the SLOS syncer.
 */
static int
slsfs_wakeup_syncer(int is_exiting)
{
	mtx_lock(&slos.slsfs_sync_lk);
        /* Don't sync again if already in progress. */
        /* XXX Maybe exit if it's already in progress? How do we
         * serialize writes and syncs? (If a write after the last
         * sync but before this one doesn't get  written by the former
         * then we have to go through with the latter).
         */
	if (slos.slsfs_syncing) {
		DBUG("Wait\n");
		cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	}

	slos.slsfs_syncing = 1;
	if (is_exiting) {
		slos.slsfs_sync_exit = 1;
	}

        /* The actual wakeup. */
	wakeup(&slos.slsfs_syncing);

        /* Wait until the syncer notifies us it's done. */
	cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	mtx_unlock(&slos.slsfs_sync_lk);

	return (0);
}

/*
 * Unmount the filesystem from the kernel.
 */
static int
slsfs_unmount(struct mount *mp, int mntflags)
{
	struct slsfs_device *sdev;
	struct slsfsmount *smp;
	struct slos * slos;
	int error;
	int flags = 0;

	DBUG("UNMOUNTING\n");
	smp = mp->mnt_data;
	sdev = smp->sp_sdev;
	slos = smp->sp_slos;
	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
	}

        /* Remove all SLOS_related vnodes. */
	error = vflush(mp, 0, flags | SKIPSYSTEM, curthread);
	if (error) {
		return (error);
	}

	DBUG("Syncing\n");
        /*
         * Flush the data to the disk. We have already removed all
         * vnodes, so this is going to be the last flush we need.
         */
	slsfs_wakeup_syncer(1);

	DBUG("Flushed all active vnodes\n");
        /* Remove the mounted device. */
	error = slsfs_unmount_device(sdev);
	if (error) {
		return (error);
	}

	cv_destroy(&slos->slsfs_sync_cv);
	mtx_destroy(&slos->slsfs_sync_lk);
	slsfs_freemntinfo(mp);

	DBUG("Freeing mount info\n");
	mp->mnt_data = NULL;
	DBUG("Changing mount flags\n");

        /* We've removed the local filesystem info. */
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	return (0);
}

/*
 *
 */
static void
slsfs_init_vnode(struct vnode *vp, uint64_t ino)
{
	struct slos_node *mp = SLSVP(vp);
	if (ino == SLOS_ROOT_INODE) {
		vp->v_vflag |= VV_ROOT;
	} else if (ino == SLOS_INODES_ROOT) {
		vp->v_type = VREG;
		vp->v_vflag |= VV_SYSTEM;
	} else {
		vp->v_type = IFTOVT(mp->sn_ino.ino_mode);
	}
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
	struct slos_node * svnode;
	struct thread *td;

	td = curthread;
	vp = NULL;
	none = NULL;

        /* Make sure the inode does not already have a vnode. */
	error = vfs_hash_get(mp, ino, LK_EXCLUSIVE, td, &vp,
	    NULL, NULL);
	if (error) {
		return (error);
	}

        /* If we do have a vnode already, return it. */
	if (vp != NULL) {
		*vpp = vp;
		return (0);
	}

        /* Bring the inode in memory. */
	error = slsfs_get_node(&slos, ino, &svnode);
	if (error) {
		*vpp = NULL;
		return (error);
	}

        /* Get a new blank vnode. */
	error = getnewvnode("slsfs", mp, &sls_vnodeops, &vp);
	if (error) {
		DBUG("Problem getting new inode\n");
		*vpp = NULL;
		return (error);
	}

        /*
         * If the vnode is not the root, which is managed directly
         * by the SLOS, add it to the mountpoint.
         */
	if (ino != SLOS_INODES_ROOT) {
		lockmgr(&vp->v_lock, LK_EXCLUSIVE, NULL);
		error = insmntque(vp, mp);
		if (error) {
			DBUG("Problem queing root into mount point\n");
			*vpp = NULL;
			return (error);
		}
	}

	svnode->sn_slos = &slos;
	vp->v_data = svnode;
	vp->v_bufobj.bo_ops = &bufops_slsfs;
	vp->v_bufobj.bo_bsize = IOSIZE(svnode);
	slsfs_init_vnode(vp, ino);

	DBUG("%lu\n", IOSIZE(svnode));

        /* Again, if we're not the inode metanode, add bookkeeping. */
        /*
         * XXX Why? This means we would get a different vnode every time
         * we try to open the metanode.
         */
	if (ino != SLOS_INODES_ROOT) {
		error = vfs_hash_insert(vp, ino, 0, td, &none, NULL, NULL);
		if (error || none != NULL) {
			DBUG("Problem with vfs hash insert\n");
			*vpp = NULL;
			return (error);
		}
	}

	DBUG("vget(%p) ino = %ld\n", vp, ino);

	*vpp = vp;
	return (0);

}


/*
 * Wakeup the syncer for a regular sync.
 */
static int
slsfs_sync(struct mount *mp, int waitfor)
{
	slsfs_wakeup_syncer(0);
	return (0);
}

static struct vfsops slsfs_vfsops = {
	.vfs_init =		slsfs_init,
	.vfs_uninit =		slsfs_uninit,
	.vfs_root =		slsfs_root,
	.vfs_statfs =		slsfs_statfs,
	.vfs_mount =		slsfs_mount,
	.vfs_unmount =		slsfs_unmount,
	.vfs_vget =		slsfs_vget,
	.vfs_sync =		slsfs_sync
};

VFS_SET(slsfs_vfsops, slsfs, 0);
MODULE_VERSION(slsfs, 0);
