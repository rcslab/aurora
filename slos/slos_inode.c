#include <sys/param.h>

#include <sys/sbuf.h>
#include <sys/time.h>
#include <sys/buf.h>
#include <sys/bio.h>

#include <slos.h>
#include <slos_inode.h>
#include <slsfs.h>

#include "slos_alloc.h"
#include "slos_btree.h"
#include "slos_bnode.h"
#include "slos_io.h"
#include "slosmm.h"
#include "slsfs.h"
#include "slsfs_buf.h"

static int
compare_vnode_t(const void *k1, const void *k2)
{
	const size_t * key1 = (const size_t *)k1;
	const size_t * key2 = (const size_t *)k2;

	if (*key1 > *key2) {
		return 1;
	} else if (*key1 < *key2) {
		return -1;
	} 
	return 0;
}

/*
 * Import an inode from the OSD.
 */
static int
slos_iread(struct slos *slos, uint64_t blkno, struct slos_inode **inop)
{
	struct slos_inode *ino;
	int error;

	ino = malloc(slos->slos_sb->sb_bsize, M_SLOS, M_WAITOK);

	/* Read the bnode from the disk. */
	error = slos_readblk(slos, blkno, ino); 
	if (error != 0) {
		free(ino, M_SLOS);
		return error;
	}

	/* 
	 * If we can't read the magic, we read
	 * something that's not an inode. 
	 */
	if (ino->ino_magic != SLOS_IMAGIC) {
		free(ino, M_SLOS);
		return EINVAL;
	}

	*inop = ino;

	return 0;
}

/*
 * Export an inode to the OSD.
 */
static int
slos_iwrite(struct slos *slos, struct slos_inode *ino)
{
	if (ino->ino_magic != SLOS_IMAGIC)
		return EINVAL;

	/* Write bnode to the disk. */
	return slos_writeblk(slos, ino->ino_blk, ino); 
}

static int
slos_readino(struct slos *slos, uint64_t pid, struct slos_inode *ino)
{
	struct buf *buf;
	int error;
	VOP_LOCK(slos->slsfs_inodes, LK_SHARED);

	error = slsfs_bread(slos->slsfs_inodes, pid, BLKSIZE(slos), NULL, &buf);
	if (error) {
		return (error);
	}

	memcpy(ino, buf->b_data, sizeof(*ino));
	VOP_UNLOCK(slos->slsfs_inodes, 0);

	brelse(buf);
	return (0);
}

/* Read an existing node into the buffer cache. */
int
slos_newnode(struct slos* slos, uint64_t pid, struct slos_node **vpp)
{
	struct slos_node *vp;
	int error;

	/* Read the inode from disk. */
	vp = malloc(sizeof(struct slos_node), M_SLOS, M_WAITOK | M_ZERO);
	error = slos_readino(slos, pid, &vp->sn_ino);
	if (error) {
		free(vp, M_SLOS);
		return (error);
	}
	vp->sn_pid = vp->sn_ino.ino_pid;
	vp->sn_uid = vp->sn_ino.ino_uid;
	vp->sn_gid = vp->sn_ino.ino_gid;
	memcpy(vp->sn_procname, vp->sn_ino.ino_procname, SLOS_NAMELEN);

	vp->sn_ctime = vp->sn_ino.ino_ctime;
	vp->sn_mtime = vp->sn_ino.ino_mtime;
	vp->sn_blk = vp->sn_ino.ino_blk;
	vp->sn_slos = slos;
	vp->sn_status = SLOS_VALIVE;
	vp->sn_refcnt = 0;

	fbtree_init(slos->slsfs_dev, vp->sn_ino.ino_btree.offset, sizeof(uint64_t), 
	    sizeof(diskptr_t), &compare_vnode_t, "Vnode Tree", 0, &vp->sn_tree);

	mtx_init(&vp->sn_mtx, "slosvno", NULL, MTX_DEF);

	*vpp = vp;
	return 0;
}

/*
 * Create the in-memory vnode from the on-disk inode.
 * The inode needs to have no existing vnode in memory.
 */
struct slos_node *
slos_vpimport(struct slos *slos, uint64_t inoblk)
{
	struct slos_inode *ino;
	struct slos_node *vp = NULL;
	int error;

	/* Read the inode from disk. */
	DBUG("BLKNO %lu\n", inoblk);
	error = slos_iread(slos, inoblk, &ino);
	if (error != 0) {
		DBUG("ERROR READING");
		return NULL;
	}
	vp = malloc(sizeof(struct slos_node), M_SLOS, M_WAITOK | M_ZERO);
	/* Move each field separately, translating between the two. */
	vp->sn_ino = *ino;
	vp->sn_pid = ino->ino_pid;
	vp->sn_uid = ino->ino_uid;
	vp->sn_gid = ino->ino_gid;
	memcpy(vp->sn_procname, ino->ino_procname, SLOS_NAMELEN);

	vp->sn_ctime = ino->ino_ctime;
	vp->sn_mtime = ino->ino_mtime;

	vp->sn_blk = ino->ino_blk;
	vp->sn_slos = slos;

	vp->sn_status = SLOS_VALIVE;
	/* The refcount will be incremented by the caller. */
	vp->sn_refcnt = 0;

	fbtree_init(slos->slsfs_dev, ino->ino_btree.offset, sizeof(uint64_t),
	    sizeof(diskptr_t), &compare_vnode_t, "VNode Tree", 0, &vp->sn_tree);

	mtx_init(&vp->sn_mtx, "slosvno", NULL, MTX_DEF);

	free(ino, M_SLOS);
	return vp;
}

/* Export an updated vnode into the on-disk inode. */
int
slos_vpexport(struct slos *slos, struct slos_node *vp)
{
	struct slos_inode *ino;
	int error;

	/* Bring in the inode from the disk. */
	error = slos_iread(slos, vp->sn_blk, &ino);
	if (error != 0)
		return error;

	/* Update the inode's mutable elements. */
	ino->ino_ctime = vp->sn_ctime;
	ino->ino_mtime = vp->sn_mtime;

	memcpy(ino->ino_procname, vp->sn_procname, SLOS_NAMELEN);

	ino->ino_records = DISKPTR(vp->sn_records->root, 1);

	/* Write the inode back to the disk. */
	error = slos_iwrite(slos, ino);
	if (error != 0) {
		free(ino, M_SLOS);
		return error;
	}

	vp->sn_ino = *ino;

	free(ino, M_SLOS);
	return 0;
}

/* Free an in-memory vnode. */
void
slos_vpfree(struct slos *slos, struct slos_node *vp)
{
	/* Free its resources. */
	mtx_lock(&vp->sn_mtx);
	mtx_destroy(&vp->sn_mtx);

	free(vp, M_SLOS);
}

/* Free the on-disk resources for an inode. */
static void
slos_ifree(struct slos *slos, struct slos_node *vp)
{
#if 0
	uint64_t prevroot;
	int error;

	/*
	 * Detach both the inode and the vnode from 
	 * the SLOS-wide indexes.
	 */

	/* Remove the inode from the inodes btree. */
	error = btree_delete(slos->slos_inodes, vp->sn_pid);
	if (error != 0)
		goto error;

	/* 
	 * If we changed the root of the inode 
	 * btree, update the superblock. 
	 */
	if (slos->slos_inodes->root != slos->slos_sb->sb_inodes.offset) {
		prevroot = slos->slos_sb->sb_inodes.offset; 
		slos->slos_sb->sb_inodes.offset = slos->slos_inodes->root;

		error = slos_sbwrite(slos);
		if (error != 0) {
			/* If it failed, roll back the change of root. */
			slos->slos_sb->sb_inodes.offset = prevroot;
			goto error;
		}
	}

	/*
	 * Free all in-memory resources.
	 */

	/* 
	 * While destroying the records themselves 
	 * in here would be messy, destroying the
	 * records btree is not.
	 */

	/* Traverse the tree, one record at a time. */
	/* 
	 * XXX Free record-related blocks. This can be left to the GC
	 * by doing the "deferred transactions" thing.
	 */

	/* Give the root node of the btree back to the allocator. */
	slos_free(slos->slos_alloc, DISKPTR_BLOCK(vp->sn_records->root));

	/* Free the inode itself. */
	slos_free(slos->slos_alloc, DISKPTR_BLOCK(vp->sn_blk));

	/* 
	 * Remove the in-memory resources. 
	 */

	slos_vpfree(slos, vp);

	btree_discardelem(slos->slos_inodes);

	return;
error:
	btree_keepelem(vp->sn_records);
	btree_keepelem(slos->slos_inodes);

	/* 
	 * No matter whether we failed, we still
	 * don't need the in-memory vnode. Free it. 
	 */
	slos_vpfree(slos, vp);
#endif
	/* XXX Implement */
}

/* Create an inode for the process with the given PID. */
int
slos_icreate(struct slos *slos, uint64_t pid, uint16_t mode)
{
	int error;
	struct buf *bp;
	struct timespec tv;
	struct fnode_iter iter;
	struct slos_inode ino;
	diskptr_t ptr;

	struct vnode *root_vp = slos->slsfs_inodes;
	struct slos_node *svp = SLSVP(root_vp);
	size_t blksize = IOSIZE(svp);

        DBUG("Creating inode %lx\n", pid);
	// For now we will use the blkno for our pids
	error = slsfs_lookupbln(svp, pid, &iter);
	if (error) {
		return (error);
	}

	VOP_LOCK(root_vp, LK_EXCLUSIVE);
	if (ITER_ISNULL(iter) || ITER_KEY_T(iter, uint64_t) != pid) {
		ITER_RELEASE(iter);
	} else {
		ITER_RELEASE(iter);
		VOP_UNLOCK(root_vp, 0);
                DBUG("Failed to create inode %lx\n", pid);
		return EINVAL;
	}

	struct uio io;
	struct iovec iov;
	iov.iov_base = &ino;
	iov.iov_len = sizeof(ino);
	slos_uioinit(&io, pid * blksize, UIO_WRITE, &iov, 1);

	ino.ino_pid = pid;
	ino.ino_ctime = tv.tv_sec;
	ino.ino_ctime_nsec = tv.tv_nsec;
	ino.ino_mtime = tv.tv_sec;
	ino.ino_mtime_nsec = tv.tv_nsec;
	ino.ino_nlink = 0;
	ino.ino_flags = 0;
	ino.ino_blk = -1;
	ino.ino_magic = SLOS_IMAGIC;
	ino.ino_mode = mode;
	ino.ino_asize = 0;
	ino.ino_size = 0;
	ino.ino_blocks = 0;
	error = ALLOCATEBLK(slos, BLKSIZE(slos), &ptr);
	if (error) {
		return (error);
	}
	ino.ino_btree = ptr;
	VOP_WRITE(root_vp, &io, 0, NULL);

	bp = getblk(slos->slsfs_dev, ptr.offset, BLKSIZE(slos), 0, 0, 0);
	if (error) {
		return (error);
	}
	vfs_bio_clrbuf(bp);
	slsfs_bdirty(bp);
	VOP_UNLOCK(root_vp, 0);

	// We will use this private pointer as a way to change this ino with 
	// the proper ino blk number when it syncs
        DBUG("Created inode %lx\n", pid);

	return 0;
}

int
slos_iremove(struct slos *slos, uint64_t pid)
{
#if 0
	struct slos_node *vp;
	struct slos_diskptr inoptr; 
	int error;

	SLOS_LOCK(slos);

	/* 
	 * If there are no open versions of the
	 * inode, destroy right now, otherwise
	 * the inode is going to be destroyed
	 * then the refcount goes to 0.
	 */
	vp = slos_vhtable_find(slos, pid);
	if (vp == NULL) { 
		/* Get the on-disk position of the inode. */
		error = btree_search(slos->slos_inodes, pid, &inoptr);
		if (error != 0) {
			SLOS_UNLOCK(slos);
			return error;
		}

		/* 
		 * We need the vnode to be able to
		 * traverse the inode's resources.
		 */
		vp = slos_vpimport(slos, inoptr.offset);
		if (vp == NULL) {
			SLOS_UNLOCK(slos);
			return EIO;
		}
		/* Free both in-memory and on-disk resources. */
		SLOS_UNLOCK(slos);
		slos_ifree(slos, vp);

		return (0);
	} else {
		/* 
		 * If there are still open inodes, we have to
		 * wait until they are done to free the resources.
		 * Mark the vnode as being deleted. 
		 */
		vp->sn_status = SLOS_VDEAD;

	}

	SLOS_UNLOCK(slos);
#endif

	/* Not yet implemented. */
	return (ENOSYS);
}

/*
 * Open a vnode corresponding
 * to an on-disk inode. 
 */
struct slos_node *
slos_iopen(struct slos *slos, uint64_t pid)
{
	DBUG("Opening Inode %lx\n", pid);
	struct slos_node *vp = NULL;
	int error;
	/* We should not hold this essentially global lock in this object. We 
	 * can run into a scenario where when we search through the btree, we 
	 * end up having to sleep while
	 * we wait for the buffer, current fix is to allow sleeping on this lock*/
	if (pid == SLOS_INODES_ROOT) {
		vp = slos_vpimport(slos, slos->slos_sb->sb_root.offset);
		if (vp == NULL) {
			SLOS_UNLOCK(slos);
			return NULL;
		}
		vp->sn_refcnt += 1;
	} else {
		/* Create a vnode for the inode. */
		error = slos_newnode(slos, pid, &vp);
		if (error) {
			return NULL;
		}
	}
	DBUG("Opened Inode %lx\n", pid);

	return vp; 
}

/* 
 * Close an open vnode.
 */
int
slos_iclose(struct slos *slos, struct slos_node *vp)
{
	int error;

	SLOS_LOCK(slos);
	/* Export the disk inode if it's still alive. */
	if (vp->sn_status == SLOS_VALIVE) {
		error = slos_vpexport(slos, vp);
		if (error != 0) {
			SLOS_UNLOCK(slos);
			return error;
		}
	}
	SLOS_UNLOCK(slos);

	mtx_lock(&vp->sn_mtx);
	vp->sn_refcnt -= 1;
	mtx_unlock(&vp->sn_mtx);
	if (vp->sn_refcnt == 0) {
		/* The file has been deleted, free it. */
		if (vp->sn_status == SLOS_VDEAD) {
			/* Free both the inode and the vnode. */
			DBUG("iclose vdead\n");
			slos_ifree(slos, vp);
		} else {
			/* Destroy only the in-memory vnode. */
			DBUG("iclose vpfree\n");
			slos_vpfree(slos, vp);
		}
	} 

	return 0;
}

// We assume the svp struct is locked
void 
slos_timechange(struct slos_node *svp)
{
	struct timespec ts;
	struct slos_inode *ino;

	ino = &svp->sn_ino;

	vfs_timestamp(&ts);
	svp->sn_ctime = ts.tv_sec;
	svp->sn_mtime = ts.tv_sec;
	ino->ino_ctime = ts.tv_sec;
	ino->ino_ctime_nsec = ts.tv_nsec;
	ino->ino_mtime = ts.tv_sec;
	ino->ino_mtime_nsec = ts.tv_nsec;
}

// We assume that svp is under the VOP_LOCK, we currently just check if the svp 
// being updated is the root itself
int 
slos_updatetime(struct slos_node *svp)
{
	mtx_lock(&svp->sn_mtx);
	slos_timechange(svp);
	mtx_unlock(&svp->sn_mtx);
	return (0);
}

int 
slos_updateroot(struct slos_node *svp)
{
	struct buf *bp;
	int error;
	VOP_LOCK(slos.slsfs_inodes, LK_EXCLUSIVE);
	error = slsfs_bread(slos.slsfs_inodes, svp->sn_pid, IOSIZE(svp), NULL, &bp);
	if (error) {
		VOP_UNLOCK(slos.slsfs_inodes, 0);
		return (error);
	}
	memcpy(bp->b_data, &svp->sn_ino, sizeof(svp->sn_ino));
	slsfs_bdirty(bp);
	VOP_UNLOCK(slos.slsfs_inodes, 0);
	return (0);
}


#ifdef SLOS_TESTS

/*
 * Randomized test for the inodes. Each round
 * we either create, open, close, or destroy an
 * inode. As long as everything runs correctly,
 * we will be able ot see the created files across
 * mounts and unmounts.
 */

#define ITERATIONS  100000
#define POPEN	    80	
#define PCLOSE	    80
#define PCREATE	    80
#define PDESTROY    80

#define VNOVACANT   0
#define VNOTAKEN    1

#define TESTLEN	    4

#define PIDNO	    4096

/*
 * Iterator for going through all entries of the
 * inode btree. Each time we iterate, we open
 * a vnode that we can use to operate on the vnode.
 * The previously iterated vnode is closed.
 */
struct slos_viter {
	uint64_t nextpid;
	struct slos_node *curvp;
};

static struct slos_viter *
slos_viter_begin(struct slos *slos)
{
	struct slos_viter *viter;

	viter = malloc(sizeof(*viter), M_SLOS, M_WAITOK);
	viter->nextpid = 0;
	viter->curvp = NULL;

	return viter;
}

static struct slos_node *
slos_viter_iter(struct slos *slos, struct slos_viter *viter)
{
	struct slos_node *vp;
	struct slos_diskptr inoptr;
	int error;

	if (viter->curvp != NULL) {
		/* Close the previously iterated vnode. */
		error = slos_iclose(slos, viter->curvp);
		if (error != 0)
			return NULL;

		/* Overwrite the pointer to avoid closing twice. */
		viter->curvp = NULL;
	}

	/* Get the next key equal to or larger than the current one. */
	error = btree_keymax(slos->slos_inodes, &viter->nextpid, &inoptr);
	if (error != 0)
		return NULL;

	/* Open the new vnode. */
	vp = slos_iopen(slos, viter->nextpid);
	if (vp == NULL)
		return NULL;

	/* Update the iterator. */
	viter->nextpid += 1;
	viter->curvp = vp;

	return vp;
}

static int
slos_viter_end(struct slos *slos, struct slos_viter *viter)
{
	int error;

	if (viter->curvp != NULL) {
		/* Close the previously iterated vnode. */
		error = slos_iclose(slos, viter->curvp);
		if (error != 0)
			return error;

		/* Overwrite the pointer to avoid closing twice. */
		viter->curvp = NULL;
	}

	free(viter, M_SLOS);

	return 0;
}

#endif /* SLOS_TESTS */
