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
#include <sys/stat.h>

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

#include "../slos/slos_bootalloc.h"
#include "../slos/slos_alloc.h"

#include "slsfs_subr.h"
#include "slsfs.h"
#include "slsfs_dir.h"

static MALLOC_DEFINE(M_SLSFSMNT, "slsfs_mount", "SLS mount structures");

static vfs_root_t	slsfs_root;
static vfs_statfs_t	slsfs_statfs;
static vfs_vget_t	slsfs_vget;
static vfs_sync_t	slsfs_sync;
extern struct buf_ops bufops_slsfs;

static const char *slsfs_opts[] = { "from" };

static int
slsfs_mount_device(struct vnode *devvp, struct mount *mp, struct slsfs_device **slsfsdev)
{
	struct slsfs_device * sdev;
	struct g_provider *pp;
	struct g_consumer *cp;
	int ronly;

	DBUG("Mounting Device\n");
	ronly = 0;
	*slsfsdev = NULL;
	devvp = slos.slos_vp;
	pp = slos.slos_pp;
	cp = slos.slos_cp;

	sdev = malloc(sizeof(struct slsfs_device), M_SLSFSMNT, M_WAITOK | M_ZERO);
	sdev->refcnt = 1;
	sdev->devvp = devvp;
	sdev->gprovider = pp;
	sdev->gconsumer = cp;
	mtx_init(&sdev->g_mtx, "slsfsmtx", NULL, MTX_DEF);

	sdev->devsize = pp->mediasize;
	sdev->devblocksize = pp->sectorsize;
	*slsfsdev = sdev;
	DBUG("Mounting Device Done\n");

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
slsfs_valloc(struct vnode *dvp, mode_t mode, struct ucred * creds, struct vnode **vpp)
{
	int error;
	uint64_t pid = 0;

	error = slsfs_new_node(VPSLOS(dvp), mode, &pid);
	if (error) {
		return (error);
	}

	error = slsfs_vget(dvp->v_mount, pid, LK_EXCLUSIVE, vpp);
	if (error) {
		return (error);
	}

	return (0);
}

static int 
slsfs_mountfs(struct vnode *devvp, struct mount *mp)
{
	struct slsfsmount *smp = NULL;
	struct slsfs_device *slsfsdev = NULL;
	int error, ronly;
	char * from;

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

	if (devvp->v_rdev->si_iosize_max != 0)
		mp->mnt_iosize_max = devvp->v_rdev->si_iosize_max;

	if (mp->mnt_iosize_max > MAXPHYS)
		mp->mnt_iosize_max = MAXPHYS;

	from = vfs_getopts(mp->mnt_optnew, "from", &error);
	if (error)
		goto error;

	smp = (struct slsfsmount *) malloc(sizeof(struct slsfsmount), M_SLSFSMNT, M_WAITOK | M_ZERO);
	error = slsfs_mount_device(devvp, mp, &slsfsdev);
	if (error) {
		goto error;
	}
	mp->mnt_data = smp;
	smp->sp_vfs_mount = mp;
	smp->sp_sdev = slsfsdev;
	smp->sp_ronly = ronly;
	smp->sls_valloc = &slsfs_valloc;
	smp->sp_slos = &slos;

	KASSERT(smp->sp_slos != NULL, ("Null slos"));
	if (error)
		goto error;

	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	smp->sp_sdev = slsfsdev;
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

	return (error); 
}

static int 
slsfs_mount(struct mount *mp)
{
	DBUG("Mounting slsfs\n");
	struct vnode *devvp;
	//struct nameidata nd;
	struct vfsoptlist *opts;
	int error = 0;
	char * from;

	opts = mp->mnt_optnew;
	vfs_filteropt(opts, slsfs_opts);
	if (mp->mnt_flag & MNT_UPDATE) {
		// TODO: Update of Mount
	}

	from = vfs_getopts(opts, "from", &error);
	if (error != 0) {
		return (error);
	}

	devvp = slos.slos_vp;
	if (!vn_isdisk(devvp, &error)) {
		DBUG("Dbug is not a disk\n");
		vput(devvp);
		return (error);
	}

	if (error) {
		vput(devvp);
		return (error);
	}

	vfs_getnewfsid(mp);
	error = slsfs_mountfs(devvp, mp);
	if (error) {
		return (error);
	}
	vfs_mountedfrom(mp, from);
	DBUG("Mount done\n");

	struct vnode *vp = NULL;
	VFS_ROOT(mp, LK_EXCLUSIVE, &vp);
	if (vp == NULL) {
		return (EIO);
	}

	if (SLSINO(SLSVP(vp))->ino_nlink < 2) {
	    DBUG("Retrieved Root\n");
	    size_t rno;
	    error = slos_rcreate(SLSVP(vp), SLOSREC_DATA, &rno);
	    if (error) {
		return (error);
	    }
	    DBUG("Creating Data record for root inode - %lu\n", rno);
	    slsfs_init_dir(vp, vp, NULL);
	}
	VOP_UNLOCK(vp, 0);

	return (error);
}

static int
slsfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct vnode *vp;
	int error;
	error = VFS_VGET(mp, SLOS_ROOT_INODE, flags, &vp);
	if (error)
		return (error);

	*vpp = vp;
	return(0);
}

static int
slsfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct slsfs_device	    *slsdev;
	struct slsfsmount	    *smp;

	DBUG("Statfs called\n");
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

static void
slsfs_unmount_device(struct slsfs_device *sdev)
{
	sdev->refcnt--;
	if (sdev->refcnt >= 1)
		return;

	mtx_destroy(&sdev->g_mtx);
	free(sdev, M_SLSFSMNT);
	DBUG("Device Unmounted\n");

	return;
}

static void
free_mount_info(struct mount *mp)
{
	DBUG("Destroying slsmount info\n");
	struct slsfsmount *smp = TOSMP(mp);

	if (mp == NULL)
		return;

	free(smp, M_SLSFSMNT);
	DBUG("Destroyed slsmount info\n");
}

static int
slsfs_unmount(struct mount *mp, int mntflags)
{
	DBUG("UNMOUNTING\n");
	struct slsfs_device *sdev;
	struct slsfsmount *smp;
	struct slos * slos;
	int error;
	int flags = 0;

	smp = mp->mnt_data; 
	sdev = smp->sp_sdev;
	slos = smp->sp_slos;
	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
	}

	error = vflush(mp, 0, flags | SKIPSYSTEM, curthread);
	if (error) {
		return (error);
	}

	DBUG("Flushed all active vnodes\n");
	slsfs_unmount_device(sdev);
	DBUG("Destroyed device struct\n");
	free_mount_info(mp);
	DBUG("Freeing mount info\n");
	mp->mnt_data = 0;
	DBUG("Changing mount flags\n");
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	return (0);
}

static void
slsfs_init_vnode(struct vnode *vp, uint64_t ino) 
{
	struct slos_node *mp = SLSVP(vp);
	if (ino == SLOS_ROOT_INODE) {
		vp->v_vflag |= VV_ROOT;
	}
	vp->v_type = IFTOVT(mp->sn_ino->ino_mode);
}

static int
slsfs_vget(struct mount *mp, ino_t ino, int flags, struct vnode **vpp)
{
	int error;
	struct vnode *vp;
	struct vnode *none;
	struct slos_node * svnode;
	struct thread *td;
	struct slos *slos = MPTOSLOS(mp);

	td = curthread;
	vp = NULL;
	none = NULL;

	error = vfs_hash_get(mp, ino, LK_EXCLUSIVE, td, &vp,
	    NULL, NULL);
	if (error) {
		return (error);
	}

	if (vp != NULL) {
		*vpp = vp;
		return (error);
	}

	error = slsfs_getnode(slos, ino, &svnode);
	if (error) {
		*vpp = NULL;
		return (error);
	}

	error = getnewvnode("slsfs", mp, &sls_vnodeops, &vp);
	if (error) {
		DBUG("Problem getting new inode\n");
		*vpp = NULL;
		return (error);
	}

	lockmgr(&vp->v_lock, LK_EXCLUSIVE, NULL);
	error = insmntque(vp, mp);
	if (error) {
		DBUG("Problem queing root into mount point\n");
		*vpp = NULL;
		return (error);
	}

	svnode->sn_slos = slos;
	vp->v_data = svnode;
	vp->v_bufobj.bo_ops = &bufops_slsfs;
	vp->v_bufobj.bo_bsize = IOSIZE(svnode);
	slsfs_init_vnode(vp, ino);

	error = vfs_hash_insert(vp, ino, 0, td, &none, NULL, NULL);
	if (error || none != NULL) {
		DBUG("Problem with vfs hash insert\n");
		*vpp = NULL;
		return (error);
	}
	*vpp = vp;
	return (0);

}

static int
slsfs_sync(struct mount *mp, int waitfor)
{
	struct vnode *vp, *mvp;
	struct thread *td;
	struct bufobj *bo;
	int error;

	td = curthread;

	DBUG("SYNCING\n");
	MNT_VNODE_FOREACH_ACTIVE(vp, mp, mvp) {
		if(VOP_ISLOCKED(vp)) {
			VI_UNLOCK(vp);
			continue;
		}

		if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK | LK_NOWAIT, td) != 0) {
			continue;
		}
		bo = &vp->v_bufobj;
		if (bo->bo_dirty.bv_cnt) {
			error = slsfs_sync_vp(vp);
			if (error) {
				/*
				 * XXX: For error handling of this case, we 
				 * will be optimistic for now and assume we can 
				 * attempt this buffer on the next sync.  May 
				 * not be true.
				 */
				vput(vp);
				return (error);
			}
		} 
		vput(vp);

	}
	DBUG("SYNCING DONE\n");
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
MODULE_DEPEND(slsfs, slos, 0, 0, 10000);
MODULE_VERSION(slsfs, 0);
