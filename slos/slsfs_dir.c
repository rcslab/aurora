#include <sys/types.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/namei.h>
#include <sys/uio.h>

#include <slsfs.h>
#include <slos_inode.h>
#include <slos_io.h>
#include <slosmm.h>

#include "slsfs_dir.h"
#include "slsfs_subr.h"
#include "slsfs_buf.h"

int
slsfs_add_dirent(struct vnode *vp, uint64_t ino, char *nameptr, long namelen, uint8_t type)
{
	struct dirent *dir;
	struct dirent *pdir;
	struct buf *bp = NULL;
	int error;
	size_t off;

	struct slos_node *svp = SLSVP(vp);
	size_t blksize = IOSIZE(svp);
	size_t blks = SLSINO(svp).ino_blocks;

	if (blks) {
		error = slsfs_bread(vp, blks - 1, blksize, curthread->td_ucred, &bp);
		if (error) {
			return (error);
		}
		off = 0;
		while ((off + sizeof(struct dirent)) < blksize) {
			pdir = (struct dirent *)(bp->b_data + off);
			if (pdir->d_reclen == 0) {
				pdir = NULL;
				break;
			}
			off += sizeof(struct dirent) ;
		}

		if ((off + sizeof(struct dirent)) > blksize) {
			brelse(bp);
			bp = NULL;
		}
	} 

	if (!bp) {
		error = slsfs_bcreate(vp, blks, blksize, NULL, &bp);
		if (error) {
			DBUG("Problem creating buffer at %lu\n", blks);
			return (error);
		}
		blks += 1;
		SLSINO(svp).ino_blocks = blks;
		// We are pre adding the dirent we will be adding to this directory
		// Actual size is the full block allocated to it;
		SLSINO(svp).ino_asize = SLSINO(svp).ino_blocks * blksize;
		off = 0;
	}

	dir = (struct dirent *)(bp->b_data + off);
	dir->d_fileno = ino;
	dir->d_off = ((blks - 1) * blksize) + off; 
	memcpy(dir->d_name, nameptr, namelen);
	dir->d_type = type;
	dir->d_reclen = sizeof(struct dirent);
	dir->d_namlen = namelen;
	DBUG("Placing directory %s at offset %lu\n",  dir->d_name, dir->d_off);

	slsfs_bdirty(bp);
	if (type == DT_DIR) {
		SLSINO(svp).ino_nlink++;
	}
	SLSINO(svp).ino_size = dir->d_off + sizeof(struct dirent);
	// XXX This is actually incorrect -- we are flushing an update to disk 
	// when we havnt actually made the change to directory on disk, this 
	// probably cleans itself up when we make the changes to inodes though 
	// on disk. (Root Inode, its buffers will the inode itself so we will 
	// dirty those buffers)
	error = slos_updatetime(svp);
	if (error) {
		DBUG("Problem syncing inode update");
	}

	return (error);
}

int
slsfs_init_dir(struct vnode *dvp, struct vnode *vp, struct componentname *name) 
{
	/* Create the pointer to current directory */
	int error;

	struct slos_node *svp = SLSVP(vp);
	int ino = VINUM(dvp);

	error = slsfs_add_dirent(vp, ino, "..", 2, DT_DIR);
	if (error) {
		DBUG("Problem adding ..\n");
		return (error);
	}

	error = slsfs_add_dirent(vp, ino, ".", 1, DT_DIR);
	if (error) {
		DBUG("Problem adding .\n");
		return (error);
	}

	// Catch the edge case of the root directory
	if (name != NULL) {
		DBUG("Adding %s\n", name->cn_nameptr);
		error = slsfs_add_dirent(dvp, 
		    SVINUM(svp), name->cn_nameptr, name->cn_namelen, DT_DIR);
	}

	return (error);
}

/* 
 * Unlink the child file from the parent
 */
int
slsfs_unlink_dir(struct vnode *dvp, struct vnode *vp, struct componentname *name)
{
	struct dirent dir;
	struct dirent *last, *del;
	struct buf *del_bp, *last_bp;
	size_t del_off, last_off, final_off;
	size_t del_bno, last_bno;
	int error;

	struct slos_node *svp = SLSVP(vp);
	struct slos_node *sdvp = SLSVP(dvp);
	size_t size = SLSVPSIZ(sdvp);
	size_t blksize = IOSIZE(svp);

	error = slsfs_lookup_name(dvp, name, &dir);
	if (error) {
		return (error);
	}

	del_bno = dir.d_off / blksize;
	del_off = dir.d_off % blksize;
	last_bno = SLSINO(sdvp).ino_blocks - 1; 
	last_off = (size % blksize);

	error = slsfs_bread(dvp, last_bno, blksize, curthread->td_ucred, &last_bp);
	if (error) {
		brelse(last_bp);
		return (error); 
	}

	KASSERT(last_off != 0, ("We should not be at 0"));

	last_off -= sizeof(struct dirent);
	last = (struct dirent *)(last_bp->b_data + last_off);
	// Buffer will be the same, so lets not read in another buffer
	if (last_bno == del_bno) {
		del_bp = last_bp;
	} else {
		error = slsfs_bread(dvp, del_bno, blksize, curthread->td_ucred, &del_bp);
		if (error) {
			brelse(del_bp);
			brelse(last_bp);
			return (error); 
		}
	}

	//If we decided to read in a block we assert that there should be
	KASSERT(last != NULL, ("No way this should happen"));
	// This means that the last entry IS the one being deleted
	if (last->d_namlen == name->cn_namelen && 
	    !strncmp(last->d_name, name->cn_nameptr, name->cn_namelen)) {
		KASSERT(del_bp == last_bp, ("If they are the same they should have been in the same block"));
		KASSERT(strcmp(dir.d_name, last->d_name) == 0, ("Should be the same Directory"));
		bzero(last, sizeof(struct dirent));
	} else {
		// Turn the deleted dirent into the last dirent;
		del = (struct dirent *)(del_bp->b_data + del_off);
		KASSERT(strcmp(dir.d_name, del->d_name) == 0, ("Should be the same Directory"));
		final_off = del->d_off;
		*del = *last;
		del->d_off = final_off;
		// Delete the last dirent;
		bzero(last, sizeof(struct dirent));
	}

	// Last element in the block so have to move back
	if (last_off == 0) {
		// Remove the offset in the file
		SLSINO(sdvp).ino_blocks--;
		DBUG("Last of the block, removing block from tree - %lu blocks left\n", SLSINO(sdvp).ino_blocks);
		bundirty(last_bp);
		brelse(last_bp);

		KASSERT(SLSINO(sdvp).ino_blocks != 0, ("Should never occur"));
		SLSINO(sdvp).ino_size = ((SLSINO(sdvp).ino_blocks) * blksize) - (blksize % sizeof(struct dirent));
	} else {
		SLSINO(sdvp).ino_size -= sizeof(struct dirent);
		slsfs_bdirty(last_bp);
	}

	if (last_bp != del_bp) {
		slsfs_bdirty(del_bp);
	}

	// Update the links and size;
	if (vp->v_type == VDIR) {
		SLSINO(sdvp).ino_nlink--;
	}

	// Update inode size to disk
	slos_updatetime(sdvp);
	error  = slos_updateroot(sdvp);
	if (error) {
		return (error);
	}

	return (0);
}

int
slsfs_lookup_name(struct vnode *vp, struct componentname *name, struct dirent *dir_p) 
{
	struct dirent *dir;
	struct buf *bp = NULL;
	int error;
	size_t off;

	struct slos_node *svp = SLSVP(vp);
	size_t blksize = IOSIZE(svp);
	size_t blks = SLSINO(svp).ino_blocks;

	KASSERT(name->cn_nameptr != NULL, ("We require the name right now to lookup dirs"));
	for (int i = 0; i < blks; i++){
		error = slsfs_bread(vp, i, BLKSIZE(svp->sn_slos), curthread->td_ucred, &bp);
		if (error) {
			return (error);
		}
		off = 0;
		while ((off + sizeof(struct dirent)) < blksize) {
			dir = (struct dirent *)(bp->b_data + off);
			if (dir->d_reclen == 0) {
				dir = NULL;
				break;
			}
			DBUG("Directory lookup %s\n", dir->d_name);
			if ((name->cn_namelen == dir->d_namlen) &&
				strncmp(name->cn_nameptr, dir->d_name, name->cn_namelen) == 0) {
				DBUG("Directory found\n");
				*dir_p = *dir;
				brelse(bp);
				return (0);
			}

			off += sizeof(struct dirent);
		}
		brelse(bp);
	} 

	return (EINVAL);
}

int slsfs_update_dirent(struct vnode *tdvp, struct vnode *fvp, struct vnode *tvp)
{
	return (0);
}
