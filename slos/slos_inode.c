#include <sys/param.h>

#include <sys/sbuf.h>
#include <sys/time.h>

#include <slos.h>
#include <slos_inode.h>
#include <slsfs.h>

#include "slos_alloc.h"
#include "slos_btree.h"
#include "slos_bnode.h"
#include "slos_io.h"
#include "slosmm.h"

/* 
 * Initialize the hashtable that holds all open vnodes. There is no need
 * for locking, since the SLOS does not export any methods to its users.
 */
int
slos_vhtable_init(struct slos *slos)
{
	slos->slos_vhtable = malloc(sizeof(*slos->slos_vhtable), M_SLOS, M_WAITOK);
	
	slos->slos_vhtable->vh_table = (struct slos_vnlist *)
	    hashinit(VHTABLE_MAX, M_SLOS, &slos->slos_vhtable->vh_hashmask);
	if (slos->slos_vhtable->vh_table == NULL) {
	    free(slos->slos_vhtable, M_SLOS);
	    return ENOMEM;
	}
	
	return 0;
}

/*
 * Destroy the open vnodes hashtable. This function takes in the SLOS locked,
 * and leaves it so. This operation fails if there are open vnodes.
 */
int
slos_vhtable_fini(struct slos *slos)
{
	struct slos_vnlist *bucket;
	int i;

	/* 
	 * Any vnodes that exist in the hashtable
	 * are still open by a user. If we exited,
	 * we would at least cause a crash, and in
	 * the worst case we could have use-after-free.
	 *
	 * The solution is to wait for the opened 
	 * files to be closed.
	 */    
	for (i = 0; i <= slos->slos_vhtable->vh_hashmask; i++) {
	    bucket = &slos->slos_vhtable->vh_table[i];
	    if (!LIST_EMPTY(bucket))
		return EBUSY;
	}

	/* If all went well, destroy the vnode table. */
	hashdestroy(slos->slos_vhtable->vh_table, M_SLOS, 
		slos->slos_vhtable->vh_hashmask);
	free(slos->slos_vhtable, M_SLOS);
	slos->slos_vhtable = NULL;

	return 0;
}

/* 
 * Find a vnode corresponding to the given PID. This function takes
 * in the SLOS locked, and leaves it so.
 */
struct slos_node *
slos_vhtable_find(struct slos *slos, uint64_t pid)
{
	struct slos_vnlist *bucket;
	struct slos_node *vp;

	bucket = &slos->slos_vhtable->vh_table[pid & slos->slos_vhtable->vh_hashmask];
	LIST_FOREACH(vp, bucket, sn_entries) {
	    if (vp->sn_pid == pid)
		return vp;
	}
	
	return NULL;
}

/* 
 * Add a vnode to the hashtbale. This function takes
 * in the SLOS locked, and leaves it so.
 */
void
slos_vhtable_add(struct slos *slos, struct slos_node *vp)
{
	struct slos_vnlist *bucket;

	bucket = 
	    &slos->slos_vhtable->vh_table[vp->sn_pid & slos->slos_vhtable->vh_hashmask];
	LIST_INSERT_HEAD(bucket, vp, sn_entries);
}

/* 
 * Remove a vnode from the hashtable. This function takes
 * in the SLOS locked, and leaves it so.
 */
void
slos_vhtable_remove(struct slos *slos, struct slos_node *vp)
{
	/* 
	 * All vnodes are in the hashtable, so
	 * we don't need to search for it.
	 *
	 * What would be nice is a static assertion
	 * that the vnode is indeed in the hashtable.
	 */
	LIST_REMOVE(vp, sn_entries);
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

/*
 * Create the in-memory vnode from the on-disk inode.
 * The inode needs to have no existing vnode in memory.
 */
struct slos_node *
slos_vpimport(struct slos *slos, uint64_t inoblk)
{
	struct slos_inode *ino;
	struct slos_node *vp;
	int error;

	/* Read the inode from disk. */
	error = slos_iread(slos, inoblk, &ino);
	if (error != 0)
	    return NULL;

	vp = malloc(sizeof(struct slos_node), M_SLOS, M_WAITOK | M_ZERO);
	/* 
	 * Move each field separately, 
	 * translating between the two. 
	 */
	vp->sn_ino = ino;
	vp->sn_pid = ino->ino_pid;
	vp->sn_uid = ino->ino_uid;
	vp->sn_gid = ino->ino_gid;
	memcpy(vp->sn_procname, ino->ino_procname, SLOS_NAMELEN);

	vp->sn_ctime = ino->ino_ctime;
	vp->sn_mtime = ino->ino_mtime;

	/* Initialize the records btree. */
	vp->sn_records = btree_init(slos, ino->ino_records.offset, ALLOCMAIN);
	if (vp->sn_records == NULL) {
	    free(ino, M_SLOS);
	    free(vp, M_SLOS);
	    return NULL;
	}

	vp->sn_blk = ino->ino_blk;
	vp->sn_slos = slos;

	vp->sn_status = SLOS_VALIVE;
	/* The refcount will be incremented by the caller. */
	vp->sn_refcnt = 0;
	/* Add to the open vnode hashtable. */
	slos_vhtable_add(slos, vp);
	/* Initialize the mutex used by record operations. */
	mtx_init(&vp->sn_mtx, "slosvno", NULL, MTX_DEF);

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

	free(vp->sn_ino, M_SLOS);
	vp->sn_ino = ino;
	return 0;
}

/* Free an in-memory vnode. */
void
slos_vpfree(struct slos *slos, struct slos_node *vp)
{
	/* Remove the vnode from the hashtable. */
	slos_vhtable_remove(slos, vp);

	/* Free its resources. */
	mtx_lock(&vp->sn_mtx);
	mtx_destroy(&vp->sn_mtx);

	/* Destroy the in-memory records btree. */
	btree_discardelem(vp->sn_records);
	btree_destroy(vp->sn_records);

	free(vp->sn_ino, M_SLOS);
	free(vp, M_SLOS);
}

/* Free the on-disk resources for an inode. */
static void
slos_ifree(struct slos *slos, struct slos_node *vp)
{
	uint64_t prevroot;
	int error;

	mtx_lock(&slos->slos_mtx);

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

	mtx_unlock(&slos->slos_mtx);

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
}

/* Create an inode for the process with the given PID. */
int
slos_icreate(struct slos *slos, uint64_t pid, uint16_t mode)
{
	struct slos_inode *ino = NULL;
	struct bnode *bnode = NULL;
	struct slos_diskptr diskptr = DISKPTR_NULL;
	struct slos_diskptr recptr = DISKPTR_NULL;
	uint64_t prevroot;
	struct timespec tv;
	int error;

	vfs_timestamp(&tv);

	/* Initialize inode block */
	mtx_lock(&slos->slos_mtx);

	/* 
	 * Search the inodes btree for an existing 
	 * inode with the same key.
	 */
	DBUG("iCreate Allocation search - %lu\n", pid);
	error = btree_search(slos->slos_inodes, pid, &diskptr);
	if (error == 0) {
	    /* 
	     * Make the disk pointer NULL again so that we 
	     * don't free the existing vnode in the error path.
	     */
	    diskptr = DISKPTR_NULL;
	    error = EINVAL;
	    DBUG("iCreate Allocation pid found\n");
	    goto error;
	} else if (error != 0 && error != EINVAL) {
	    DBUG("iCreate Allocation other error\n");
	    goto error;
	}

	DBUG("iCreate Allocation - Diskptr\n");
	diskptr = slos_alloc(slos->slos_alloc, 1);
	DBUG("Diskptr allocated - %ld , %ld\n", diskptr.offset, diskptr.size);
	if (diskptr.offset == 0) {
	    error = ENOSPC;
	    goto error;
	}

	/* 
	 * Create and initialize the inode. The 
	 * GID, UID, and name fields have
	 * to be set by the caller.
	 */
	ino = malloc(slos->slos_sb->sb_bsize, M_SLOS, M_WAITOK);
	ino->ino_pid = pid;

	ino->ino_ctime = tv.tv_sec;
	ino->ino_ctime_nsec = tv.tv_nsec;
	ino->ino_mtime = tv.tv_sec;
	ino->ino_mtime_nsec = tv.tv_nsec;
	ino->ino_link_num = 0;

	ino->ino_flags = 0;
	ino->ino_blk = diskptr.offset;
	ino->ino_magic = SLOS_IMAGIC;
	ino->ino_mode = mode;
	ino->ino_asize = 0;
	ino->ino_size = 0;

	/* Get a block for the records btree. */
	DBUG("iCreate Allocation - Recptr\n");
	recptr = slos_alloc(slos->slos_alloc, 1);
	DBUG("Recptr allocated - %ld , %ld\n", recptr.offset, recptr.size);
	if (recptr.offset == 0) {
	    error = ENOSPC;
	    goto error;
	}

	/* Actually write the bnode to disk. */
	bnode = bnode_alloc(slos, recptr.offset, 
		sizeof(struct slos_diskptr), BNODE_EXTERNAL);
	error = bnode_write(slos, bnode);
	if (error != 0)
	    goto error;
	
	/* Have the inode point to the btree root. */
	ino->ino_records.offset = recptr.offset;
	ino->ino_records.size = 1;

	/* Write the inode to disk. */
	error = slos_iwrite(slos, ino);
	if (error != 0)
	    goto error;

	/* 
	 * Add the inode to the inodes btree. They
	 * are keyed by the PID of the process
	 * that this inode corresponds to.
	 */
	error = btree_insert(slos->slos_inodes, 
		ino->ino_pid, &DISKPTR_BLOCK(ino->ino_blk));
	if (error != 0)
	    goto error;

	/* If the root of the inodes btree changed, update the superblock. */
	if (slos->slos_inodes->root != slos->slos_sb->sb_inodes.offset) {
	    prevroot = slos->slos_sb->sb_inodes.offset;
	    slos->slos_sb->sb_inodes.offset = slos->slos_inodes->root;
	    error = slos_sbwrite(slos);
	    if (error != 0) {
		slos->slos_sb->sb_inodes.offset = prevroot;
		btree_keepelem(slos->slos_inodes);
		goto error;
	    }
	}

	btree_discardelem(slos->slos_inodes);
	free(bnode, M_SLOS);
	free(ino, M_SLOS);

	mtx_unlock(&slos->slos_mtx);

	return 0;

error:
	btree_keepelem(slos->slos_inodes);
	slos_free(slos->slos_alloc, diskptr);
	slos_free(slos->slos_alloc, recptr);

	free(bnode, M_SLOS);
	free(ino, M_SLOS);

	mtx_unlock(&slos->slos_mtx);
	
	return error;
}

int
slos_iremove(struct slos *slos, uint64_t pid)
{
	struct slos_node *vp;
	struct slos_diskptr inoptr; 
	int error;

	mtx_lock(&slos->slos_mtx);

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
		mtx_unlock(&slos->slos_mtx);
		return error;
	    }

	    /* 
	     * We need the vnode to be able to
	     * traverse the inode's resources.
	     */
	    vp = slos_vpimport(slos, inoptr.offset);
	    if (vp == NULL) {
		mtx_unlock(&slos->slos_mtx);
		return EIO;
	    }
	    /* Free both in-memory and on-disk resources. */
	    mtx_unlock(&slos->slos_mtx);
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

	mtx_unlock(&slos->slos_mtx);

	return (0);
}

/*
 * Open a vnode corresponding
 * to an on-disk inode. 
 */
struct slos_node *
slos_iopen(struct slos *slos, uint64_t pid)
{
	DBUG("Opening Inode %lu\n", pid);
	struct slos_node *vp = NULL;
	struct slos_diskptr inoptr;
	int error;

	/* We should not hold this essentially global lock in this object. We can run into a 
	 * scenario where when we search through the btree, we end up having to sleep while
	 * we wait for the buffer, current fix is to allow sleeping on this lock*/
	SLOS_LOCK(slos);
	vp = slos_vhtable_find(slos, pid);
	if (vp != NULL) {
	    /* Found, update refcount and return. */
	    vp->sn_refcnt += 1;
	    DBUG("Found vp in slos\n");
	    SLOS_UNLOCK(slos);
	    return vp;
	}

	/* 
	 * If we're here, no one else has opened
	 * this inode. Open it here.
	 */
	error = btree_search(slos->slos_inodes, pid, &inoptr);
	if (error != 0) {
	    DBUG("Could not find %lu in btree\n", pid);
	    SLOS_UNLOCK(slos);
	    return NULL;
	}

	/* Create a vnode for the inode. */
	DBUG("Creating vnode from disk\n");
	vp = slos_vpimport(slos, inoptr.offset);
	if (vp == NULL) {
	    DBUG("Could not create vnode for %lu\n", pid);
	    SLOS_UNLOCK(slos);
	    return NULL;
	}
	vp->sn_refcnt += 1;
	SLOS_UNLOCK(slos);

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

/*
 * Return a structure that holds 
 * statistics about the inode.
 */
/* XXX Fix prototype */
struct slos_node *
slos_istat(struct slos *slos, uint64_t ino)
{
	/* XXX Implement */

	return 0;
}

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

// We assume the svp struct is locked
void 
slos_timechange(struct slos_node *svp)
{
    struct timespec ts;
    struct slos_inode *ino;

    ino = svp->sn_ino;

    vfs_timestamp(&ts);
    svp->sn_ctime = ts.tv_sec;
    svp->sn_mtime = ts.tv_sec;
    ino->ino_ctime = ts.tv_sec;
    ino->ino_ctime_nsec = ts.tv_nsec;
    ino->ino_mtime = ts.tv_sec;
    ino->ino_mtime_nsec = ts.tv_nsec;
}

int 
slos_iupdate(struct slos_node *svp)
{
    struct slos_inode *ino;
    int error;

    mtx_lock(&svp->sn_mtx);
    ino = svp->sn_ino;
    slos_timechange(svp);
    error = slos_iwrite(svp->sn_slos, ino);
    mtx_unlock(&svp->sn_mtx);

    return error;
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

int
slos_test_inode(void)
{
	struct slos_node *vp;
	struct slos_viter *viter;
	uint8_t *pids, *open;
	int operation;
	char **names;
	uint64_t pid;
	int error = 0;
	int opens, closes, creates, removes;
	int i, j;

	/* Initialize the counters. */
	opens = closes = creates = removes = 0;
	
	/* An array that shows which vnodes exist. */
	pids = malloc(sizeof(*pids) * PIDNO, M_SLOS, M_WAITOK);
	memset(pids, VNOVACANT, sizeof(*pids) * PIDNO);

	/* 
	 * An array that shows which vnodes are open, and how 
	 * many times. Removed vnodes can still be open. 
	 */
	open = malloc(sizeof(*open) * PIDNO, M_SLOS, M_WAITOK | M_ZERO);
    
	/* We're going to use the names of the inodes to check for correctness. */
	names = malloc(sizeof(*names) * PIDNO, M_SLOS, M_WAITOK | M_ZERO);
	/* The names array elements have character SLOS_NAMELEN + 1 always be \0. */
	for (i = 0; i < PIDNO; i++)
	    names[i] = malloc(sizeof(*names[i]) * (SLOS_NAMELEN + 1), M_SLOS, M_WAITOK | M_ZERO);

	printf("SLOS vnodes before test:\n");

	/* Traverse the inodes btree, taking note of all existing inodes. */
	viter = slos_viter_begin(&slos);
	for (;;) {
	    /* Get the next inode. */
	    vp = slos_viter_iter(&slos, viter);
	    if (vp == NULL)
		break;

	    pids[vp->sn_pid] = VNOTAKEN;
	    memcpy(names[vp->sn_pid], vp->sn_procname, 
		    sizeof(*names[vp->sn_pid]) * SLOS_NAMELEN);

	    printf("(%ld) %s\n", vp->sn_pid, vp->sn_procname);
	}
	slos_viter_end(&slos, viter);

	for (i = 0; i < ITERATIONS; i++) {
	    operation = random() % (POPEN + PCLOSE + PCREATE + PDESTROY);
	    pid = random() % PIDNO;

	    if (operation < POPEN) {
		/* 
		 * Open an existing file. If the file
		 * doesn't exist don't do anything. 
		 */
		if (pids[pid] == VNOVACANT)
		    continue;
    
		vp = slos_iopen(&slos, pid);
		if (vp == NULL) {
		    printf("ERROR: slos_iopen returned NULL\n");
		    break;
		}

		/* If it is already open, it must have a valid name. */
		if (open[pid] > 0) {
		    if (strncmp(vp->sn_procname, names[pid], TESTLEN) != 0) {
			printf("ERROR: PID %ld has name %s, expected %s", 
				pid, vp->sn_procname, names[pid]);
			break;
		    }
		}

		/* Give the process a new name. */
		for (j = 0; j < TESTLEN; j++) 
		    vp->sn_procname[j] = '0' + (random() % 9);
		vp->sn_procname[j] = '\0';
		strncpy(names[pid], vp->sn_procname, TESTLEN + 1);

		open[pid] += 1;
		opens += 1;
		//printf("Opened %ld\n", pid);
	    
		//printf("Opened PID %ld (current opens: %d)\n", pid, open[pid]);
	    } else if (operation < POPEN + PCLOSE) {
		/* 
		 * Close an existing open file. If the file
		 * doesn't exist or is closed, don't do anything. 
		 */
		if (pids[pid] == VNOVACANT || open[pid] == 0)
		    continue;

		//printf("Closing PID %ld (current opens: %d)\n", pid, open[pid]);

		vp = slos_vhtable_find(&slos, pid);
		if (vp == NULL) {
		    printf("ERROR: PID %ld not found in vhtable\n", pid);
		    break;
		}

		if (strncmp(vp->sn_procname, names[pid], TESTLEN) != 0) {
		    printf("ERROR: PID %ld has name %s, expected %s", 
			    pid, vp->sn_procname, names[pid]);
		    break;
		}

		error = slos_iclose(&slos, vp);
		if (error != 0) {
		    printf("ERROR: Closing PID %ld failed with %d\n", pid, error);
		    break; 
		}
		
		open[pid] -= 1;
		closes += 1;
		//printf("Closed PID %ld\n", pid);
	    
	    } else if (operation < POPEN + PCLOSE + PCREATE) {
		/* 
		 * Create a new file. The file has to be both
		 * nonexistent on disk and closed in memory.
		 */
		if (pids[pid] == VNOTAKEN || open[pid] != 0)
		    continue;

		error = slos_icreate(&slos, pid);
		if (error != 0) {
		    printf("ERROR: Creating PID %ld failed with %d\n", pid, error);
		    break;
		}

		pids[pid] = VNOTAKEN;
		
		/* 
		 * We have to open and close the file to be able to set the 
		 * filename. This couples the tests, but is unavoidable;
		 */

		vp = slos_iopen(&slos, pid);
		if (vp == NULL) {
		    printf("ERROR: slos_iopen returned NULL\n");
		    break;
		}

		/* Give the process a new name. */
		for (j = 0; j < TESTLEN; j++) 
		    vp->sn_procname[j] = '0' + (random() % 9);
		vp->sn_procname[j] = '\0';
		strncpy(names[pid], vp->sn_procname, TESTLEN);

		error = slos_iclose(&slos, vp);
		if (error != 0) {
		    printf("ERROR: Closing PID %ld failed with %d\n", pid, error);
		    break; 
		}

		creates += 1;
		//printf("Created PID %ld, name %s\n", pid, names[pid]);
	    } else {
		/* Destroy an existing file. */
		if (pids[pid] == VNOVACANT)
		    continue;

		error = slos_iremove(&slos, pid);
		if (error != 0) {
		    printf("ERROR: Removing PID %ld failed with %d\n", pid, error);
		    break;
		}

		pids[pid] = VNOVACANT;
		removes += 1;
		//printf("Removed PID %ld\n", pid);
	    }


	}

	printf("SLOS vnodes after test:\n");

	/* Iterate the inode btree once more, listing the vnodes that exist. */
	viter = slos_viter_begin(&slos);
	for (;;) {
	    /* Get the next inode. */
	    vp = slos_viter_iter(&slos, viter);
	    if (vp == NULL)
		break;

	    /* We only care about alive nodes. */
	    if (vp->sn_status == SLOS_VALIVE)
		printf("(%ld) %s\n", vp->sn_pid, vp->sn_procname);

	    /* 
	     * Close all open vnodes so that we 
	     * can clean up the vhtable.
	     */
	    pid = vp->sn_pid;
	    while (open[pid] > 0) {
		error = slos_iclose(&slos, vp);
		if (error != 0) {
		    printf("ERROR: slos_iclose for PID %ld failed with %d", 
			    vp->sn_pid, error);
		    break;
		}

		open[pid] -= 1;
	    }
	}
	slos_viter_end(&slos, viter);

	printf("Iterations: %d\nOpens: %d\t Closes: %d\nCreates: %d\tRemoves: %d\n",
		    i, opens, closes, creates, removes);

	for (i = 0; i < PIDNO; i++)
	    free(names[i], M_SLOS);
	free(names, M_SLOS);
	free(pids, M_SLOS);
	free(open, M_SLOS);
	
	return error;
}

#endif /* SLOS_TESTS */
