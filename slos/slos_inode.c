#include <sys/param.h>

#include <sys/sbuf.h>
#include <sys/time.h>

#include "../include/slos.h"
#include "slos_alloc.h"
#include "slos_btree.h"
#include "slos_inode.h"
#include "slos_internal.h"
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
struct slos_vnode *
slos_vhtable_find(struct slos *slos, uint64_t pid)
{
	struct slos_vnlist *bucket;
	struct slos_vnode *vp;

	bucket = &slos->slos_vhtable->vh_table[pid & slos->slos_vhtable->vh_hashmask];
	LIST_FOREACH(vp, bucket, vno_entries) {
	    if (vp->vno_pid == pid)
		return vp;
	}
	
	return NULL;
}

/* 
 * Add a vnode to the hashtbale. This function takes
 * in the SLOS locked, and leaves it so.
 */
void
slos_vhtable_add(struct slos *slos, struct slos_vnode *vp)
{
	struct slos_vnlist *bucket;

	bucket = 
	    &slos->slos_vhtable->vh_table[vp->vno_pid & slos->slos_vhtable->vh_hashmask];
	LIST_INSERT_HEAD(bucket, vp, vno_entries);
}

/* 
 * Remove a vnode from the hashtable. This function takes
 * in the SLOS locked, and leaves it so.
 */
void
slos_vhtable_remove(struct slos *slos, struct slos_vnode *vp)
{
	/* 
	 * All vnodes are in the hashtable, so
	 * we don't need to search for it.
	 *
	 * What would be nice is a static assertion
	 * that the vnode is indeed in the hashtable.
	 */
	LIST_REMOVE(vp, vno_entries);
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
struct slos_vnode *
slos_vpimport(struct slos *slos, uint64_t inoblk)
{
	struct slos_inode *ino;
	struct slos_vnode *vp;
	int error;

	/* Read the inode from disk. */
	error = slos_iread(slos, inoblk, &ino);
	if (error != 0)
	    return NULL;

	vp = malloc(sizeof(*vp), M_SLOS, M_WAITOK | M_ZERO);
	/* 
	 * Move each field separately, 
	 * translating between the two. 
	 */
	vp->vno_pid = ino->ino_pid;
	vp->vno_uid = ino->ino_uid;
	vp->vno_gid = ino->ino_gid;
	memcpy(vp->vno_procname, ino->ino_procname, SLOS_NAMELEN);

	vp->vno_ctime = ino->ino_ctime;
	vp->vno_mtime = ino->ino_mtime;

	/* Initialize the records btree. */
	vp->vno_records = btree_init(slos, ino->ino_records.offset, ALLOCMAIN);
	if (vp->vno_records == NULL) {
	    free(ino, M_SLOS);
	    free(vp, M_SLOS);
	    return NULL;
	}

	vp->vno_blk = ino->ino_blk;
	vp->vno_lastrec = ino->ino_lastrec;

	vp->vno_status = SLOS_VALIVE;
	/* The refcount will be incremented by the caller. */
	vp->vno_refcnt = 0;
	/* Add to the open vnode hashtable. */
	slos_vhtable_add(slos, vp);
	/* Initialize the mutex used by record operations. */
	mtx_init(&vp->vno_mtx, "slosvno", NULL, MTX_DEF);

	free(ino, M_SLOS);

	return vp;
}

/* Export an updated vnode into the on-disk inode. */
int
slos_vpexport(struct slos *slos, struct slos_vnode *vp)
{
	struct slos_inode *ino;
	int error;

	/* Bring in the inode from the disk. */
	error = slos_iread(slos, vp->vno_blk, &ino);
	if (error != 0)
	    return error;

	/* Update the inode's mutable elements. */ 
	ino->ino_ctime = vp->vno_ctime;
	ino->ino_mtime = vp->vno_mtime;

	memcpy(ino->ino_procname, vp->vno_procname, SLOS_NAMELEN);

	ino->ino_lastrec = vp->vno_lastrec;
	ino->ino_records = DISKPTR(vp->vno_records->root, 1);

	/* Write the inode back to the disk. */
	error = slos_iwrite(slos, ino);
	if (error != 0) {
	    free(ino, M_SLOS);
	    return error;
	}

	free(ino, M_SLOS);
	return 0;
}

/* Free an in-memory vnode. */
static void
slos_vpfree(struct slos *slos, struct slos_vnode *vp)
{
	/* Remove the vnode from the hashtable. */
	slos_vhtable_remove(slos, vp);

	/* Free its resources. */
	mtx_destroy(&vp->vno_mtx);

	/* Destroy the in-memory records btree. */
	btree_discardelem(vp->vno_records);
	btree_destroy(vp->vno_records);

	free(vp, M_SLOS);
}

/* Free the on-disk resources for an inode. */
static void
slos_ifree(struct slos *slos, struct slos_vnode *vp)
{
	uint64_t prevroot;
	int error;

	mtx_lock(&slos->slos_mtx);

	/*
	 * Detach both the inode and the vnode from 
	 * the SLOS-wide indexes.
	 */

	/* Remove the inode from the inodes btree. */
	error = btree_delete(slos->slos_inodes, vp->vno_pid);
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
	slos_free(slos->slos_alloc, DISKPTR_BLOCK(vp->vno_records->root));

	/* Free the inode itself. */
	slos_free(slos->slos_alloc, DISKPTR_BLOCK(vp->vno_blk));

	/* 
	 * Remove the in-memory resources. 
	 */

	slos_vpfree(slos, vp);

	mtx_unlock(&slos->slos_mtx);

	btree_discardelem(slos->slos_inodes);

	return;
error:
	btree_keepelem(vp->vno_records);
	btree_keepelem(slos->slos_inodes);

	/* 
	 * No matter whether we failed, we still
	 * don't need the in-memory vnode. Free it. 
	 */
	slos_vpfree(slos, vp);
}

/* Create an inode for the process with the given PID. */
int
slos_icreate(struct slos *slos, uint64_t pid)
{
	struct slos_inode *ino = NULL;
	struct bnode *bnode = NULL;
	struct slos_diskptr diskptr = DISKPTR_NULL;
	struct slos_diskptr recptr = DISKPTR_NULL;
	uint64_t prevroot;
	struct timeval tv;
	int error;

	/* Initialize inode block */
	mtx_lock(&slos->slos_mtx);

	/* 
	 * Search the inodes btree for an existing 
	 * inode with the same key.
	 */
	error = btree_search(slos->slos_inodes, pid, &diskptr);
	if (error == 0) {
	    error = EINVAL;
	    goto error;
	} else if (error != 0 && error != EINVAL) {
	    goto error;
	}

	diskptr = slos_alloc(slos->slos_alloc, 1);
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

	getmicrotime(&tv);
	ino->ino_ctime = (1000 * 1000 * tv.tv_sec) + tv.tv_usec;
	ino->ino_mtime = (1000 * 1000 * tv.tv_sec) + tv.tv_usec;

	ino->ino_lastrec = 0;
	ino->ino_flags = 0;
	ino->ino_blk = diskptr.offset;
	ino->ino_magic = SLOS_IMAGIC;

	
	/* Get a block for the records btree. */
	recptr = slos_alloc(slos->slos_alloc, 1);
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
	struct slos_vnode *vp;
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
	    slos_ifree(slos, vp);

	} else {
	    /* 
	     * If there are still open inodes, we have to
	     * wait until they are done to free the resources.
	     * Mark the vnode as being deleted. 
	     */
	    vp->vno_status = SLOS_VDEAD;

	}

	mtx_unlock(&slos->slos_mtx);

	return 0;
}

/*
 * Open a vnode corresponding
 * to an on-disk inode. 
 */
struct slos_vnode *
slos_iopen(struct slos *slos, uint64_t pid)
{
	struct slos_vnode *vp = NULL;
	struct slos_diskptr inoptr;
	int error;

	mtx_lock(&slos->slos_mtx);

	vp = slos_vhtable_find(slos, pid);
	if (vp != NULL) {
	    /* Found, update refcount and return. */
	    vp->vno_refcnt += 1;
	    mtx_unlock(&slos->slos_mtx);

	    return vp;
	}

	/* 
	 * If we're here, no one else has opened
	 * this inode. Open it here.
	 */
	error = btree_search(slos->slos_inodes, pid, &inoptr);
	if (error != 0) {
	    mtx_unlock(&slos->slos_mtx);
	    return NULL;
	}

	/* Create a vnode for the inode. */
	vp = slos_vpimport(slos, inoptr.offset);
	if (vp == NULL) {
	    mtx_unlock(&slos->slos_mtx);
	    return NULL;
	}

	vp->vno_refcnt += 1;

	mtx_unlock(&slos->slos_mtx);
	return vp; 
}

/* 
 * Close an open vnode.
 */
int
slos_iclose(struct slos *slos, struct slos_vnode *vp)
{
	int error;

	mtx_lock(&slos->slos_mtx);

	/* Export the disk inode if it's still alive. */
	if (vp->vno_status == SLOS_VALIVE) {
	    error = slos_vpexport(slos, vp);
	    if (error != 0) {
		mtx_unlock(&slos->slos_mtx);
		return error;
	    }
	}

	vp->vno_refcnt -= 1;
	if (vp->vno_refcnt == 0) {
	    /* The file has been deleted, free it. */
	    if (vp->vno_status == SLOS_VDEAD) {
		/* Free both the inode and the vnode. */
		slos_ifree(slos, vp);
	    } else {
		/* Destroy only the in-memory vnode. */
		slos_vpfree(slos, vp);
	    }
	} 

	mtx_unlock(&slos->slos_mtx);

	return 0;
}

/*
 * Return a structure that holds 
 * statistics about the inode.
 */
/* XXX Fix prototype */
struct slos_vnode *
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
	struct slos_vnode *curvp;
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

static struct slos_vnode *
slos_viter_iter(struct slos *slos, struct slos_viter *viter)
{
	struct slos_vnode *vp;
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
	struct slos_vnode *vp;
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

	    pids[vp->vno_pid] = VNOTAKEN;
	    memcpy(names[vp->vno_pid], vp->vno_procname, 
		    sizeof(*names[vp->vno_pid]) * SLOS_NAMELEN);

	    printf("(%ld) %s\n", vp->vno_pid, vp->vno_procname);
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
		    if (strncmp(vp->vno_procname, names[pid], TESTLEN) != 0) {
			printf("ERROR: PID %ld has name %s, expected %s", 
				pid, vp->vno_procname, names[pid]);
			break;
		    }
		}

		/* Give the process a new name. */
		for (j = 0; j < TESTLEN; j++) 
		    vp->vno_procname[j] = '0' + (random() % 9);
		vp->vno_procname[j] = '\0';
		strncpy(names[pid], vp->vno_procname, TESTLEN + 1);

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

		if (strncmp(vp->vno_procname, names[pid], TESTLEN) != 0) {
		    printf("ERROR: PID %ld has name %s, expected %s", 
			    pid, vp->vno_procname, names[pid]);
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
		    vp->vno_procname[j] = '0' + (random() % 9);
		vp->vno_procname[j] = '\0';
		strncpy(names[pid], vp->vno_procname, TESTLEN);

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
	    if (vp->vno_status == SLOS_VALIVE)
		printf("(%ld) %s\n", vp->vno_pid, vp->vno_procname);

	    /* 
	     * Close all open vnodes so that we 
	     * can clean up the vhtable.
	     */
	    pid = vp->vno_pid;
	    while (open[pid] > 0) {
		error = slos_iclose(&slos, vp);
		if (error != 0) {
		    printf("ERROR: slos_iclose for PID %ld failed with %d", 
			    vp->vno_pid, error);
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
