#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
#include <sys/namei.h>

#include <slsfs.h>
#include <slos.h>
#include <slos_inode.h>
#include <slos_record.h>
#include <slos_btree.h>
#include <slos_io.h>

#include "slsfs_dir.h"
#include "slsfs_subr.h"

#define DIR_NUMFILES(vp) (vp->sn_records->size)
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
    struct slos_inode *inode = SLSINO(slsvp);
    struct slsfsmount *smp = TOSMP(vp->v_mount);

    VATTR_NULL(vap);
    vap->va_type = IFTOVT(inode->ino_mode);
    vap->va_mode = inode->ino_mode & ~S_IFMT; 
    vap->va_nlink = inode->ino_link_num;
    vap->va_uid = 0;
    vap->va_gid = 0;
    vap->va_fsid = VNOVAL;
    vap->va_fileid = slsvp->sn_pid;
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

    slos_vpfree(svp->sn_slos, svp);
    vp->v_data = NULL;

    vnode_destroy_vobject(vp);

    return (0);
}

static int
slsfs_mkdir(struct vop_mkdir_args *args)
{
    struct vnode *dvp = args->a_dvp;
    struct slos_node *slsdvp = SLSVP(dvp);
    struct vnode **vpp = args->a_vpp;
    struct componentname *name = args->a_cnp;
    struct vattr *vap = args->a_vap;

    struct ucred creds;
    struct vnode *vp;
    int error;

    int mode = MAKEIMODE(vap->va_type, vap->va_mode);
    error = SLS_VALLOC(dvp, mode, &creds, &vp);
    if (error) {
	*vpp = NULL;
	return (error);
    } 
    
    DBUG("Initing Directory\n");
    slsfs_init_dir(slsdvp, SLSVP(vp), name);
    *vpp = vp;

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
    uint64_t rno;
    uint64_t lastrno;
    int error;

    KASSERT(slsvp->sn_slos != NULL, ("Null slos"));

    if (vp->v_type != VDIR)
	return (ENOTDIR);
    
    error = slos_lastrno(slsvp, &lastrno);
    if (error == EIO) {
	return (error);
    } else if (error == EINVAL) {
	panic("Problem directories should always have 2 records\n");
    }

    if ((io->uio_offset <= lastrno) &&
	(io->uio_resid >= sizeof(struct dirent)))
    {
	/* Create the UIO for the disk. */
	while (io->uio_offset <= lastrno) {
	    if (io->uio_offset == 0) {
		error = slos_firstrno(slsvp, &rno);
	    } else {
		/* Make getrecord - subtracting by 1 because next record will just grab that  */
		rno = io->uio_offset - 1;
		error = slos_nextrno(slsvp, &rno);
	    }

	    if (error) {
		DBUG("Problem getting record %lu - %d\n", rno, error);
		return (error);
	    }
	    struct uio auio;
	    struct iovec aiov;
	    struct dirent dir;
	    aiov.iov_base = &dir;
	    aiov.iov_len = sizeof(dir);
	    slos_uioinit(&auio, 0, UIO_READ, &aiov, 1);

	    error = slos_rread(slsvp, rno, &auio);
	    if (error) {
		DBUG("Error %d\n", error);
		return (error);
	    }

	    dir.d_reclen = GENERIC_DIRSIZ(&dir);
	    dirent_terminate(&dir);
	    if (io->uio_resid < GENERIC_DIRSIZ(&dir)) {
		break;
	    }

	    error = uiomove(&dir, dir.d_reclen, io);
	    io->uio_offset = rno + 1;
	    if (error) {
		return (error);
	    }
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
slsfs_lookup(struct vop_cachedlookup_args *args)
{
    struct vnode *dvp = args->a_dvp;
    struct vnode **vpp = args->a_vpp;
    struct componentname *cnp = args->a_cnp;
    struct vnode *vp;
    struct dirent dir;
    int namelen, nameiop, islastcn;
    char * name;
    int error = 0;

    name = cnp->cn_nameptr;
    namelen = cnp->cn_namelen;
    nameiop =  cnp->cn_nameiop;
    islastcn = cnp->cn_flags & ISLASTCN;

    /* Self directory - Must just increase reference count of dir */
    if((namelen == 1) && (name[0] == '.')) {
	VREF(dvp);
	*vpp = dvp;
    /* Check another case of the ".." directory */
    } else if (cnp->cn_flags & ISDOTDOT){
	struct componentname tmp;
	tmp.cn_nameptr = ".."; 
	tmp.cn_namelen = 2;
	error = slsfs_lookup_name(SLSVP(dvp), &tmp, &dir, NULL);
	/* Record was not found */
	if (error)
	    goto out;
	
	if (!error) {
	    error = SLS_VGET(dvp, dir.d_fileno, LK_EXCLUSIVE,  &vp);
	    if (!error) {
		*vpp = vp;
	    }
	}
    } else {
	error = slsfs_lookup_name(SLSVP(dvp), cnp, &dir, NULL);
	if (error == EINVAL) {
	    error = ENOENT;
	    /* 
	     * Are we creating or renaming the directory, and is the last
	     * name in the name component? If so change return 
	     */
	    if ((nameiop == CREATE || nameiop == RENAME) && islastcn) {
		/* Normally should check access rights but won't for now */
		DBUG("Regular name lookup - not found, but creating\n");
		cnp->cn_flags |= SAVENAME;
		error = EJUSTRETURN;
	    }
	} else {
	    /* Cases for when name is found, others to be filled in later */
	    if ((nameiop == LOOKUP) && islastcn) {
		DBUG("Lookup of file\n");
		error = SLS_VGET(dvp, dir.d_fileno, LK_EXCLUSIVE, &vp);
		if (!error) {
			*vpp = vp;
		}
	    } else if ((nameiop == DELETE) && islastcn) {
		DBUG("Delete of file\n");
		cnp->cn_flags |= SAVENAME;
		error = SLS_VGET(dvp, dir.d_fileno, LK_EXCLUSIVE, &vp);
		if (!error) {
			*vpp = vp;
		}
	    }
	}
    }

out:
    /* Cache the entry in the name cache for the future */
    if((cnp->cn_flags & MAKEENTRY) != 0) {
	cache_enter(dvp, *vpp, cnp);
    }
    return (error);
}

static int
slsfs_rmdir(struct vop_rmdir_args *args)
{
    DBUG("Removing directory\n");
    struct vnode *vp = args->a_vp;
    struct vnode *dvp = args->a_dvp;
    struct componentname *cnp = args->a_cnp;
    int error;
    
    struct slos_node *sdvp = SLSVP(dvp);
    struct slos_node *svp = SLSVP(vp);
    struct slos_inode *ivp = SLSINO(svp);

    /* Check if directory is empty */
    /* Are we mounted here*/
    if (ivp->ino_link_num > 2) {
	return (ENOTEMPTY);
    }

    if (vp->v_vflag & VV_ROOT) {
	return (EPERM);
    }

    error = slsfs_remove_node(sdvp, svp, cnp);
    if (error) {
	return (error);
    }

    cache_purge(dvp);
    cache_purge(vp);
    DBUG("Removing directory done\n");

    return (0);

}

static int
slsfs_create(struct vop_create_args *args)
{
    DBUG("Creating file\n");
    struct vnode *dvp = args->a_dvp;
    struct slos_node *slsdvp = SLSVP(dvp);
    struct vnode **vpp = args->a_vpp;
    struct componentname *name = args->a_cnp;
    struct vattr *vap = args->a_vap;

    struct ucred creds;
    struct vnode *vp;
    int error;

    int mode = MAKEIMODE(vap->va_type, vap->va_mode);
    error = SLS_VALLOC(dvp, mode, &creds, &vp);
    if (error) {
	*vpp = NULL;
	return (error);
    } 

    error = slsfs_add_dirent(slsdvp, VINUM(vp), name->cn_nameptr, name->cn_namelen, DT_REG);
    if (error == -1) {
	return (EIO);
    }

    *vpp = vp;
    return (0);
}

static int
slsfs_remove(struct vop_remove_args *args)
{
    DBUG("Removing file\n");
    struct vnode *vp = args->a_vp;
    struct vnode *dvp = args->a_dvp;
    struct componentname *cnp = args->a_cnp;
    int error;
    
    struct slos_node *sdvp = SLSVP(dvp);
    struct slos_node *svp = SLSVP(vp);

    error = slsfs_remove_node(sdvp, svp, cnp);
    if (error) {
	return (error);
    }

    cache_purge(dvp);

    DBUG("Removing file\n");

    return (0);
}

static int
slsfs_fsync(struct vop_fsync_args *args)
{
    DBUG("FSYNC\n");
    return (0);
}
static int
slsfs_print(struct vop_print_args *args)
{
    DBUG("PRINT\n");
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
	.vop_cachedlookup =	slsfs_lookup, // TODO
	.vop_close =		slsfs_close, // TODO
	.vop_create =		slsfs_create, // TODO
	.vop_getattr =		slsfs_getattr, // TODO
	.vop_inactive =		slsfs_inactive,
	.vop_ioctl =		VOP_PANIC, // TODO
	.vop_link =		VOP_PANIC, // TODO
	.vop_lookup =		vfs_cache_lookup, // TODO
	.vop_markatime =	VOP_PANIC,
	.vop_mkdir =		slsfs_mkdir, // TODO
	.vop_mknod =		VOP_PANIC, // TODO
	.vop_open =		slsfs_open, // TODO
	//.vop_pathconf =		VOP_PANIC, // TODO
	.vop_poll =		vop_stdpoll,
	.vop_print =		slsfs_print,
	.vop_readdir =		slsfs_readdir, // TODO
	.vop_readlink =		VOP_PANIC, // TODO
	.vop_reclaim =		slsfs_reclaim, // TODO
	.vop_remove =		slsfs_remove, // TODO
	.vop_rename =		VOP_PANIC, // TODO
	.vop_rmdir =		slsfs_rmdir, // TODO
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
