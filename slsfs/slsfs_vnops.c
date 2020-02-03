#include <sys/types.h>
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
#include <sys/namei.h>
#include <sys/bio.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vnode_pager.h>

#include <slsfs.h>
#include <slos.h>
#include <slos_inode.h>
#include <slos_record.h>
#include <slos_btree.h>
#include <slos_io.h>
#include <slos_record.h>

#include "slsfs_dir.h"
#include "slsfs_subr.h"
#include "slsfs_buf.h"
#include "slsfs_node.h"

#define DIR_NUMFILES(vp) (vp->sn_records->size)

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
	vap->va_nlink = inode->ino_nlink;
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
	DBUG("Reclaiming vnode %p\n", vp);
	struct slos_node *svp = SLSVP(vp);
	slos_vpfree(svp->sn_slos, svp);

	vp->v_data = NULL;
	cache_purge(vp);
	vnode_destroy_vobject(vp);
	vfs_hash_remove(vp);
	DBUG("Done reclaiming vnode %p\n", vp);

	return (0);
}

static int
slsfs_mkdir(struct vop_mkdir_args *args)
{
	struct vnode *dvp = args->a_dvp;
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

	DBUG("Initing Directory named %s\n", name->cn_nameptr);
	slsfs_init_dir(dvp, vp, name);
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
	if (DIR_NUMFILES(slsvp) >= ((uint64_t)1 << 50)) {
		panic("Why are your directories this bloody large");
	}
	uint64_t filesize = DIR_NUMFILES(slsvp) * IOSIZE(slsvp);
	vnode_create_vobject(vp, filesize, args->a_td);

	return (0);
}

static int
slsfs_readdir(struct vop_readdir_args *args)
{
	struct buf *bp;
	struct dirent dir;
	size_t blkno;
	off_t blkoff;
	size_t diroffset;
	int error = 0;

	struct vnode *vp = args->a_vp;
	struct slos_node *slsvp = SLSVP(vp);
	struct uio *io = args->a_uio;
	size_t filesize = SLSINO(slsvp)->ino_size;
	size_t blksize = IOSIZE(slsvp);

	KASSERT(slsvp->sn_slos != NULL, ("Null slos"));
	if (vp->v_type != VDIR) {
		return (ENOTDIR);
	}

	DBUG("READING DIRECTORY\n");
	if ((io->uio_offset < filesize) &&
	    (io->uio_resid >= sizeof(struct dirent)))
	{
		diroffset = io->uio_offset;
		blkno = io->uio_offset / blksize;
		blkoff = io->uio_offset % blksize;
		error = slsfs_bread(vp, blkno, curthread->td_ucred, &bp);
		if (error) {
			brelse(bp);
			DBUG("Problem reading from blk in readdir\n");
			return (error);
		}
		/* Create the UIO for the disk. */
		while (diroffset < filesize) {
			size_t anyleft = ((diroffset % blksize) + sizeof(struct dirent)) > blksize;
			if (anyleft) {
				blkoff = 0;
				blkno++;
				diroffset = blkno * blksize;
				brelse(bp);
				error = slsfs_bread(vp, blkno, curthread->td_ucred, &bp);
				if (error) {
					DBUG("Problem reading from blk in readdir in while\n");
					brelse(bp);
					return (error);
				}
			}
			if (buf_mapped(bp)) {
				KASSERT(bp->b_bcount > blkoff, ("Blkoff out of range of buffer\n"));
				dir = *((struct dirent *)(bp->b_data + blkoff));
				if (dir.d_reclen == 0) {
					break;
				}
				dir.d_reclen = GENERIC_DIRSIZ(&dir);
				dirent_terminate(&dir);
				if (io->uio_resid < GENERIC_DIRSIZ(&dir)) {
					break;
				}

				error = uiomove(&dir, dir.d_reclen, io);
				if (error) {
					DBUG("Problem moving buffer\n");
					return (error);
				}
			} else {
				brelse(bp);
				return (EIO);
			}
			diroffset += sizeof(struct dirent);
			blkoff += sizeof(struct dirent);
		}
		brelse(bp);
		io->uio_offset = diroffset;
	}

	if (args->a_eofflag != NULL) {
		*args->a_eofflag = 0;
	}

	return (error);
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
	struct dirent dir;
	int namelen, nameiop, islastcn;
	char * name;
	int error = 0;
	struct vnode *vp = NULL;
	*vpp = NULL;

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
		error = slsfs_lookup_name(dvp, &tmp, &dir);
		/* Record was not found */
		if (error)
			goto out;

		error = SLS_VGET(dvp, dir.d_fileno, LK_EXCLUSIVE,  &vp);
		if (!error) {
			*vpp = vp;
		}
	} else {
		DBUG("Looking up file %s\n", cnp->cn_nameptr);
		error = slsfs_lookup_name(dvp, cnp, &dir);
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
		} else if (error == 0) {
			/* Cases for when name is found, others to be filled in later */
			if ((nameiop == DELETE) && islastcn) {
				DBUG("Delete of file %s\n", cnp->cn_nameptr);
				error = SLS_VGET(dvp, dir.d_fileno, LK_EXCLUSIVE, &vp);
				if (!error) {
					cnp->cn_flags |= SAVENAME;
					*vpp = vp;
				}
			} else if ((nameiop == RENAME) && islastcn) {
				DBUG("Rename of file %s\n", cnp->cn_nameptr);
				panic("Rename Not implemented\n");
			} else if ((nameiop == CREATE) && islastcn) {
				DBUG("Create of file %s\n", cnp->cn_nameptr);
				error = SLS_VGET(dvp, -1, LK_EXCLUSIVE, &vp);
				if (!error) {
					cnp->cn_flags |= SAVENAME;
					*vpp = vp;
				}
			} else if ((nameiop == LOOKUP) && islastcn) {
				DBUG("Lookup of file %s\n", cnp->cn_nameptr);
				error = SLS_VGET(dvp, dir.d_fileno, LK_EXCLUSIVE, &vp);
				if (!error) {
					cnp->cn_flags |= SAVENAME;
					*vpp = vp;
				}
			} else {
				panic("nameiop corrupted value");
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

	struct slos_node *svp = SLSVP(vp);
	struct slos_inode *ivp = SLSINO(svp);

	/* Check if directory is empty */
	/* Are we mounted here*/
	if (ivp->ino_nlink > 2) {
		return (ENOTEMPTY);
	}

	if (vp->v_vflag & VV_ROOT) {
		return (EPERM);
	}

	error = slsfs_remove_node(dvp, vp, cnp);
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

	error = slsfs_add_dirent(dvp, VINUM(vp), name->cn_nameptr, name->cn_namelen, DT_REG);
	if (error == -1) {
		return (EIO);
	}

	*vpp = vp;
	return (0);
}

static int
slsfs_remove(struct vop_remove_args *args)
{
	struct vnode *vp = args->a_vp;
	struct vnode *dvp = args->a_dvp;
	struct componentname *cnp = args->a_cnp;
	DBUG("Removing file %s\n", cnp->cn_nameptr);
	int error;

	error = slsfs_remove_node(dvp, vp, cnp);
	if (error) {
		return (error);
	}

	cache_purge(dvp);

	DBUG("Removing file\n");

	return (0);
}

static int
slsfs_write(struct vop_write_args *args)
{
	struct buf *bp;
	size_t xfersize, filesize;
	uint64_t pbn;
	uint64_t off, bno;
	int error = 0;

	struct vnode *vp = args->a_vp;
	struct slos_node *svp = SLSVP(vp);
	struct slos_inode *sivp = SLSINO(svp);
	size_t blksize = IOSIZE(svp);
	struct uio *uio = args->a_uio;
	int ioflag = args->a_ioflag;

	filesize =  sivp->ino_size;

	// Check if full
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	switch(vp->v_type) {
	case VREG:
		break;
	case VDIR:
		return (EISDIR);
	case VLNK:
		break;
	default:
		panic("bad file type");
	}

	if (ioflag & IO_APPEND) {
		uio->uio_offset = filesize;
	}

	if (uio->uio_offset + uio->uio_resid > filesize)  {
		sivp->ino_size = uio->uio_offset + uio->uio_resid;
		vnode_pager_setsize(vp, sivp->ino_size);
		error = slos_iupdate(svp);
		if (error) {
			DBUG("Error updating inode\n");
			return (error);
		}
	}

	int modified = 0;
	DBUG("Writing  - %lu at offset %lu\n", uio->uio_resid, uio->uio_offset);
	while(uio->uio_resid) {
		// Grab the key thats closest to offset, but not over it
		// Mask out the lower order bits so we just have the block;
		bno = uio->uio_offset / blksize;
		error = slsfs_lookupbln(svp, bno, &pbn);
		if (error) {
			DBUG("%d\n", error);
			return (error);
		}

		off = uio->uio_offset % blksize;
		xfersize = omin(uio->uio_resid, (blksize - off));

		if (pbn == -1) {
			DBUG("Creating buf at block %lu\n", bno);
			error = slsfs_bcreate(vp, bno, blksize, &bp);
		} else {
			DBUG("Reading Buf\n");
			error = slsfs_bread(vp, bno, args->a_cred, &bp);
		}

		if (error) {
			DBUG("Problem getting buffer for write\n");
			return (error);
		}
		DBUG("Write to buf object\n");
		uiomove((char *)bp->b_data + off, xfersize, uio);
		/* One thing thats weird right now is our inodes and meta data is currently not
		 * in the buf cache, so we don't really have to worry about dirtying those buffers,
		 * but later we will have to dirty them.
		 */
		DBUG("Dirtying buf object\n");
		slsfs_bdirty(bp);
		modified++;
	}

	return (error);
}

static int
slsfs_read(struct vop_read_args *args)
{
	struct slos_inode *sivp;
	struct buf *bp;
	size_t filesize;
	uint64_t pbn;
	uint64_t off, bno;
	size_t resid;
	size_t toread;
	int error = 0;

	struct vnode *vp = args->a_vp;
	struct slos_node *svp = SLSVP(vp);
	size_t blksize = IOSIZE(svp);
	struct uio *uio = args->a_uio;

	svp = SLSVP(vp); 
	sivp = SLSINO(svp);
	filesize =  sivp->ino_size;

	// Check if full
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);

	switch(vp->v_type) {
	case VREG:
		break;
	case VDIR:
		return (EISDIR);
	case VLNK:
		break;
	default:
		panic("bad file type");
	}

	DBUG("Reading File - %lu/%lu\n", uio->uio_resid, uio->uio_offset);
	resid = omin(uio->uio_resid, (filesize - uio->uio_offset));
	DBUG("Read - RESID %lu\n", resid);
	while(resid) {
		// Grab the key thats closest to offset, but not over it
		// Mask out the lower order bits so we just have the block;
		bno = uio->uio_offset / blksize;
		error = slsfs_lookupbln(svp, bno, &pbn);
		if (error) {
			return (error);
		}

		off = uio->uio_offset % blksize;
		toread = omin(resid, (blksize - off));

		if (pbn == -1) {
			DBUG("Creating buf at block %lu\n", bno);
			error = slsfs_bcreate(vp, bno, blksize, &bp);
		} else {
			DBUG("Reading Buf\n");
			error = slsfs_bread(vp, bno, args->a_cred, &bp);
		}

		if (error) {
			return (error);
		}
		DBUG("Reading from buf object - %lu left at local blk off %lu\n", resid, off);
		error = uiomove((char *)bp->b_data + off, toread, uio);
		if (error) {
			brelse(bp);
			break;
		}
		brelse(bp);
		resid -= toread;
	}

	return (error);
}



static int
slsfs_bmap(struct vop_bmap_args *args)
{
	struct vnode *vp = args->a_vp;
	struct slsfsmount *smp = TOSMP(vp->v_mount);

	if (args->a_bop != NULL)
		*args->a_bop = &smp->sp_slos->slos_vp->v_bufobj;
	if (args->a_bnp != NULL)
		*args->a_bnp = args->a_bn;
	if (args->a_runp != NULL)
		*args->a_runp = 0;
	if (args->a_runb != NULL)
		*args->a_runb = 0;

	/*
	 * We just want to allocate for now, since allocations are persistent and get written to disk
	 * (this is obviously very slow), if we want to make this transactional we will need to to 
	 * probably do the ZFS strategy of just having this sent the physical block to the logical one
	 * and over write the buf_ops so that allocation occurs on the flush or the sync?  How would 
	 * this interact with checkpointing.  I'm thinking we will probably have all the flushes occur
	 * on a checkpoint, or before.
	 *
	 * After discussion, we believe that optimistically flushing would be a good idea, as it would 
	 * reduce the dump time for the checkpoint thus reducing latency on packets being help up 
	 * waiting for the data to be dumped to disk. Another issue we face here is that if we allocate 
	 * on each block we turn our extents and larger writes into blocks.  So I believe the best thing
	 * to do is do allocation on flush. So we will make our bmap return the logical block
	 */
	return (0);
}

static int
slsfs_fsync(struct vop_fsync_args *args)
{
	return (0);
}

static int
slsfs_print(struct vop_print_args *args)
{
	DBUG("PRINT\n");
	return (0);
}

static int
slsfs_strategy(struct vop_strategy_args *args)
{
	int error;
	struct slos_recentry entry = {};

	struct buf *bp = args->a_bp;
	struct vnode *vp = args->a_vp;

	error = slsfs_key_get(SLSVP(vp), bp->b_lblkno, &entry);
	if (error && error != EINVAL) {
		DBUG("ERROR\n");
		return (error);
	};

	/*
	 * We fill the record btree with null values on their creation (See 
	 * slsfs_bcreate), so this is to handle the case of a buffer that hasnt 
	 * been allocated yet.  If its been allocated we actually will perform
	 * the storage IO. If this is a buffer thats performing the 
	 * VOP_STRATEGY, and its a write, then this MUST be being called
	 * from within the sync context which means an allocation has already
	 * been completed.
	 *
	 * XXX: Revisit upon completely new Btrees
	 */
	if (REC_ISALLOCED(entry))  {
		bp->b_blkno = entry.diskptr.offset;
	} else {
		KASSERT(bp->b_iocmd == BIO_READ, ("This case should only happen during a read"));
		bp->b_blkno = (daddr_t) (-1);
		vfs_bio_clrbuf(bp); 
		bufdone(bp);

		return (0);
	}

	switch(bp->b_iocmd) {
	case BIO_READ:
		slos_readblk(&slos, bp->b_blkno, bp->b_data);
		break;
	case BIO_WRITE:
		slos_writeblk(&slos, bp->b_blkno, bp->b_data);
		break;
	default:
		panic("This shouldn't happen");
	}
	/* 
	 * Bufdone will finish the IO but also wakeup anyone waiting on this buf
	 * to complete its IO 
	 */
	bufdone(bp);
	return (0);
}

static int
slsfs_setattr(struct vop_setattr_args *args)
{
	return (0);
}

static int
slsfs_rename(struct vop_rename_args *args)
{
	return (0);
}




struct vop_vector sls_vnodeops = {
	.vop_default =		&default_vnodeops,
	.vop_fsync =		slsfs_fsync, 
	.vop_read =		slsfs_read, // TODO
	.vop_reallocblks =	VOP_PANIC, // TODO
	.vop_write =		slsfs_write,		
	.vop_accessx =		slsfs_accessx,
	.vop_bmap =		slsfs_bmap, // TODO
	.vop_cachedlookup =	slsfs_lookup, 
	.vop_close =		slsfs_close, 
	.vop_create =		slsfs_create, 
	.vop_getattr =		slsfs_getattr,
	.vop_inactive =		slsfs_inactive,
	.vop_ioctl =		VOP_PANIC, // TODO
	.vop_link =		VOP_PANIC, // TODO
	.vop_lookup =		vfs_cache_lookup, 
	.vop_markatime =	VOP_PANIC,
	.vop_mkdir =		slsfs_mkdir, 
	.vop_mknod =		VOP_PANIC, // TODO
	.vop_open =		slsfs_open, 
	.vop_poll =		vop_stdpoll,
	.vop_print =		slsfs_print,
	.vop_readdir =		slsfs_readdir,
	.vop_readlink =		VOP_PANIC, // TODO
	.vop_reclaim =		slsfs_reclaim, // TODO
	.vop_remove =		slsfs_remove, // TODO
	.vop_rename =		slsfs_rename, // TODO
	.vop_rmdir =		slsfs_rmdir, // TODO
	.vop_setattr =		slsfs_setattr, // TODO
	.vop_strategy =		slsfs_strategy, // TODO
	.vop_symlink =		VOP_PANIC, // TODO
	.vop_whiteout =		VOP_PANIC, // TODO
};
