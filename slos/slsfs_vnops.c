#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/dirent.h>

#include "slsfs.h"
#include "slos_internal.h"
#include "slos_inode.h"
#include "slos_record.h"

#define DIR_NUMFILES(vp) (vp->vno_records->size)
#define BLKSIZE(smp) (smp->sp_sdev->devblocksize)

static int
slsfs_inactive(struct vop_inactive_args *args)
{
    return (0);
}

static int
slsfs_getattr(struct vop_getattr_args *args)
{
    struct vnode *vp = args->a_vp;
    struct vattr *vap = args->a_vap;
    struct slos_node *slsvp = SLSVP(vp);
    struct slos_inode *inode = SLSION(slsvp);
    struct slsfsmount *smp = TOSMP(vp->v_mount);

    VATTR_NULL(vap);
    vap->va_type = IFTOVT(inode->ino_mode);
    vap->va_mode = inode->ino_mode & ~S_IFMT; 
    vap->va_nlink = inode->ino_link_num;
    vap->va_uid = 0;
    vap->va_gid = 0;
    vap->va_fsid = VNOVAL;
    vap->va_fileid = slsvp->vno_pid;
    vap->va_blocksize = smp->sp_sdev->devblocksize;
    vap->va_size = inode->ino_size;

    vap->va_atime.tv_sec = inode->ino_mtime;
    vap->va_atime.tv_nsec = inode->ino_mtime_nsec;

    vap->va_mtime.tv_sec = inode->ino_mtime;
    vap->va_mtime.tv_nsec = inode->ino_mtime_nsec;

    vap->va_ctime.tv_sec = inode->ino_ctime;
    vap->va_ctime.tv_nsec = inode->ino_ctime_nsec;

    vap->va_birthtime.tv_sec = 0;
    vap->va_gen = 0;
    vap->va_flags = inode->ino_flags;
    vap->va_rdev = NODEV;
    vap->va_bytes = inode->ino_asize;
    vap->va_filerev = 0;
    vap->va_vaflags = 0;

    return (0);
}


static int
slsfs_reclaim(struct vop_reclaim_args *args)
{
    struct vnode *vp = args->a_vp;
    struct slos_node *svp = SLSVP(vp);

    slos_vpfree(svp->vno_slos, svp);
    vp->v_data = NULL;

    vnode_destroy_vobject(vp);

    return (0);
}

static int
slsfs_mkdir(struct vop_mkdir_args *args)
{
    return (0);
}

static int
slsfs_accessx(struct vop_accessx_args *args)
{
    return (0);
}

static int
slsfs_open(struct vop_open_args *args)
{
    struct vnode *vp = args->a_vp;
    struct slos_node *slsvp = SLSVP(vp);
    struct slsfsmount *smp = TOSMP(vp->v_mount);
    uint64_t filesize = DIR_NUMFILES(slsvp) * BLKSIZE(smp);

    vnode_create_vobject(vp, filesize, args->a_td);

    return (0);
}

static int
slsfs_readdir(struct vop_readdir_args *args)
{
    struct vnode *vp = args->a_vp;
    struct slos_node *slsvp = SLSVP(vp);
    struct uio *io = args->a_uio;
    struct slos_record *rec;
    int error;

    KASSERT(slsvp->vno_slos != NULL, ("Null slos"));

    if (vp->v_type != VDIR)
	return (ENOTDIR);

    if ((io->uio_offset <= slsvp->vno_lastrec) &&
	(io->uio_resid >= sizeof(struct dirent)))
    {
	while (io->uio_offset <= slsvp->vno_lastrec) {
	    if (io->uio_offset == 0) {
		rec = slos_firstrec(slsvp);
	    } else {
		// XXX: Make getrecord - subtracting by 1 because next record will just grab that 
		rec = slos_nextrec(slsvp, io->uio_offset - 1);
	    }

	    if (rec == NULL) {
		break;
	    }

	    struct dirent * dir = (struct dirent *)rec->rec_internal_data;
	    dir->d_reclen = GENERIC_DIRSIZ(dir);
	    dirent_terminate(dir);
	    if (io->uio_resid < GENERIC_DIRSIZ(dir)) {
		break;
	    }

	    error = uiomove(dir, dir->d_reclen, io);
	    io->uio_offset = rec->rec_num + 1;
	    if (error) {
		return (error);
	    }
	    free(rec, M_SLOS);
	}
    }

    if (args->a_eofflag != NULL) {
	*args->a_eofflag = 0;
    }

    return (0);
}

static int
slsfs_close(struct vop_close_args *args)
{
    DBUG("close\n");
    return (0);
}

static int
slsfs_lookup(struct vop_lookup_args *args)
{
    DBUG("Lookup\n");
    return vfs_cache_lookup(args);
}

static int
slsfs_fsync(struct vop_fsync_args *args)
{
    DBUG("FSYNC\n");
    return (0);
}

struct vop_vector sls_vnodeops = {
	.vop_default =		&default_vnodeops,
	.vop_fsync =		slsfs_fsync, // TODO
	.vop_read =		VOP_PANIC, // TODO
	.vop_reallocblks =	VOP_PANIC, // TODO
	.vop_write =		VOP_PANIC, // TODO
	.vop_accessx =		slsfs_accessx,
	.vop_bmap =		VOP_PANIC, // TODO
	.vop_cachedlookup =	VOP_PANIC, // TODO
	.vop_close =		slsfs_close, // TODO
	.vop_create =		VOP_PANIC, // TODO
	.vop_getattr =		slsfs_getattr, // TODO
	.vop_inactive =		slsfs_inactive,
	.vop_ioctl =		VOP_PANIC, // TODO
	.vop_link =		VOP_PANIC, // TODO
	.vop_lookup =		slsfs_lookup, // TODO
	.vop_markatime =	VOP_PANIC,
	.vop_mkdir =		VOP_PANIC, // TODO
	.vop_mknod =		VOP_PANIC, // TODO
	.vop_open =		slsfs_open, // TODO
	//.vop_pathconf =		VOP_PANIC, // TODO
	.vop_poll =		vop_stdpoll,
	.vop_print =		VOP_PANIC,
	.vop_readdir =		slsfs_readdir, // TODO
	.vop_readlink =		VOP_PANIC, // TODO
	.vop_reclaim =		slsfs_reclaim, // TODO
	.vop_remove =		VOP_PANIC, // TODO
	.vop_rename =		VOP_PANIC, // TODO
	.vop_rmdir =		VOP_PANIC, // TODO
	.vop_setattr =		VOP_PANIC, // TODO
	//.vop_getwritemount =	VOP_PANIC, // TODO
/*#ifdef MAC*/
	/*.vop_setlabel =		vop_stdsetlabel_ea,*/
/*#endif*/
	.vop_strategy =		VOP_PANIC, // TODO
	.vop_symlink =		VOP_PANIC, // TODO
	.vop_whiteout =		VOP_PANIC, // TODO
/*#ifdef UFS_EXTATTR*/
	/*.vop_getextattr =	ufs_getextattr,*/
	/*.vop_deleteextattr =	ufs_deleteextattr,*/
	/*.vop_setextattr =	ufs_setextattr,*/
/*#endif*/
/*#ifdef UFS_ACL*/
	/*.vop_getacl =		ufs_getacl,*/
	/*.vop_setacl =		ufs_setacl,*/
	/*.vop_aclcheck =		ufs_aclcheck,*/
/*#endif*/
};
