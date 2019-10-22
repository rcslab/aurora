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
#include <sys/stat.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include "slos_alloc.h"
#include "slos_internal.h"
#include "slos_inode.h"
#include "slos_bootalloc.h"
#include "slos_btree.h"
#include "slos_io.h"
#include "slos_record.h"
#include "slsfs_subr.h"
#include "slsfs.h"
#include "slsfs_dir.h"
#include "slosmm.h"


struct slos slos;

static MALLOC_DEFINE(M_SLSFSMNT, "slsfs_mount", "SLS mount structures");

static vfs_root_t	slsfs_root;
static vfs_statfs_t	slsfs_statfs;
static vfs_vget_t	slsfs_vget;
static vfs_sync_t	slsfs_sync;

struct mount *mounted = NULL;
static const char *slsfs_opts[] = { "from" };

static int
slos_itreeopen(struct slos *slos)
{
	uint64_t itree;

	itree = slos->slos_sb->sb_inodes.offset;
	slos->slos_inodes = btree_init(slos, itree, ALLOCMAIN);
	if (slos->slos_inodes == NULL)
	    return EIO;

	return 0;
}

static void
slos_itreeclose(struct slos *slos)
{
	btree_discardelem(slos->slos_inodes);
	btree_destroy(slos->slos_inodes);
}

static int
slsfs_mount_device(struct vnode *devvp, struct mount *mp, struct slsfs_device **slsfsdev)
{
    struct slsfs_device * sdev;
    struct g_provider *pp;
    struct g_consumer *cp;
    struct cdev *dev;
    int error;
    int ronly;

    DBUG("Mounting Device\n");
    ronly = 0;
    *slsfsdev = NULL;
    if (mounted)
	return (0);

    vn_lock(devvp, LK_EXCLUSIVE | LK_RETRY);
    dev = devvp->v_rdev;
    dev_ref(dev);
    DROP_GIANT();
    g_topology_lock();
    error = g_vfs_open(devvp, &cp, "slsfs", ronly ? 0 : 1);
    if (error) {
	DBUG("Problem opening geom vfs\n");
	return (error);
    }
    pp = g_dev_getprovider(dev);
    g_topology_unlock();
    PICKUP_GIANT();
    VOP_UNLOCK(devvp, 0);
    if (error) {
	dev_rel(dev);
	return (error);
    }
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

static int
slsfs_create_slos(struct slsfsmount *smp)
{
    int error;

    struct slos * slosfs = &slos;
    KASSERT(slosfs != NULL, ("slos null"));
    KASSERT(smp != NULL, ("slsfs mount"));
    mtx_init(&slosfs->slos_mtx, "slosmtx", NULL, MTX_DEF);

    slosfs->slos_vp = smp->sp_sdev->devvp;
    slosfs->slos_cp = smp->sp_sdev->gconsumer;

    /* Read in the superblock. */
    DBUG("SLOS Read in Super\n");
    error = slos_sbread(slosfs);
    if (error != 0) {
	DBUG("ERROR: slos_sbread failed with %d\n", error);
	return error;
    }

    /* Set up the bootstrap allocator. */
    DBUG("SLOS Boot Init\n");
    error = slos_bootinit(slosfs);
    if (error != 0) {
	DBUG("slos_bootinit had error %d\n", error);
	return error;
    }
	
    slosfs->slos_alloc = slos_alloc_init(slosfs);
    if (slosfs->slos_alloc == NULL) {
	DBUG("ERROR: slos_alloc_init failed to set up allocator\n");
	return EINVAL;
    }

    /* Get the btree of inodes in the SLOS. */
    error = slos_itreeopen(slosfs);
    if (error != 0) {
	DBUG("ERROR: slos_itreeopen failed with %d\n", error);
	return error;
    }

    /* Set up the open vnode hashtable. */
    error = slos_vhtable_init(slosfs);
    if (error != 0) {
	DBUG("ERROR: slos_vhtable_init failed with %d\n", error);
	return error;
    }

    smp->sp_slos = slosfs;
    DBUG("SLOS Loaded.\n");

    return error;
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

    VOP_UNLOCK(devvp, 0);

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

    error = slsfs_create_slos(smp);
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
    struct nameidata nd;
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

    NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from, curthread);
    error = namei(&nd);
    if (error) {
	return (error);
    }
    NDFREE(&nd, NDF_ONLY_PNBUF);

    devvp = nd.ni_vp;
    if (!vn_isdisk(devvp, &error)) {
	    DBUG("Dbug is not a disk\n");
	    vput(devvp);
	    return (error);
    }

    error = VOP_ACCESS(devvp, VREAD, curthread->td_ucred, curthread);
    if (error) {
	error = priv_check(curthread, PRIV_VFS_MOUNT_PERM);
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
    int error;

    sdev->refcnt--;
    if (sdev->refcnt >= 1)
	return;
    
    error = vinvalbuf(sdev->devvp, V_SAVE, 0, 0);
    if (error) 
	DBUG("Problem releasing vnode buffers\n");

    DROP_GIANT();
    g_topology_lock();
    g_vfs_close(sdev->gconsumer);
    g_topology_unlock();
    PICKUP_GIANT();

    DBUG("Device closing\n");

    // Drop reference counts
    DBUG("Releasing Device vnode\n");
    vrele(sdev->devvp);
    DBUG("Releasing device r_rdev\n");
    dev_rel(sdev->devvp->v_rdev);
    DBUG("Destroying SDEV Mtx\n");

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
slsfs_destroy_slos(struct slos * fs)
{
    int error; 

    mtx_lock(&fs->slos_mtx);
    DBUG("Destroying SLOS\n");

    if (fs->slos_vhtable != NULL) {
	DBUG("Destroying SLOS vhtable\n");
	error = slos_vhtable_fini(fs);
	if (error != 0) {
	    DBUG("ERROR: slos_vhtable_fini failed with %d\n", error);
	    mtx_unlock(&fs->slos_mtx);
	    return error;
	}
    }

    if (fs->slos_inodes != NULL)
	slos_itreeclose(fs);

    DBUG("Destroying SLOS allocator\n");
    slos_alloc_destroy(fs);

    if (fs->slos_bootalloc != NULL)
	DBUG("Destroying SLOS bootalloc\n");
	slos_bootdestroy(fs);

    free(fs->slos_sb, M_SLOS);

    mtx_unlock(&fs->slos_mtx);
    mtx_destroy(&fs->slos_mtx);
    DBUG("SLOS Unloaded.\n");

    return (0);
}

static int
slsfs_unmount(struct mount *mp, int mntflags)
{
    DBUG("UNMOUNTING\n");
    struct slsfs_device *sdev;
    struct slsfsmount *smp;
    struct slos * slos;
    int error;

    smp = mp->mnt_data; 
    sdev = smp->sp_sdev;
    slos = smp->sp_slos;

    // XXX BREAKS NOW
    error = vflush(mp, 0, mntflags | SKIPSYSTEM, curthread);
    if (error)
	return (error);
    slsfs_unmount_device(sdev);
    free_mount_info(mp);
    mp->mnt_data = 0;
    MNT_ILOCK(mp);
    mp->mnt_flag &= ~MNT_LOCAL;
    mounted = NULL;
    MNT_IUNLOCK(mp);

    slsfs_destroy_slos(slos);
    
    return (0);
}

static void
slsfs_init_vnode(struct vnode *vp, uint64_t ino)
{
    struct slos_node *mp = SLSVP(vp);
    if (ino == SLOS_ROOT_INODE) {
	vp->v_vflag |= VV_ROOT;
    }
    vp->v_type = IFTOVT(mp->vno_ino->ino_mode);
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

    svnode->vno_slos = slos;
    vp->v_data = svnode;
    slsfs_init_vnode(vp, ino);

    error = vfs_hash_insert(vp, SLOS_ROOT_INODE, 0, td, &none, NULL, NULL);
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
