#include <sys/param.h>
#include <sys/vnode.h>

#include <slsfs.h>
#include <slos.h>
#include <slos_record.h>
#include <slos_inode.h>

#include "slos_bnode.h"
#include "slos_alloc.h"
#include "slos_btree.h"
#include "slos_io.h"
#include "slosmm.h"


int 
slos_freerec(struct slos_record *rec)
{
    free(rec, M_SLOS);
    return (0);
}

/* Read a record from the OSD. */
static int
slos_recdread(struct slos *slos, uint64_t blkno, struct slos_record **rpp)
{
	struct slos_record *rp;
	int error;

	rp = malloc(slos->slos_sb->sb_bsize, M_SLOS, M_WAITOK | M_ZERO);
	
	/* Read the bnode from the disk. */
	error = slos_readblk(slos, blkno, rp); 
	if (error != 0) {
	    free(rp, M_SLOS);
	    return error;
	}

	/* If we can't read the magic, we read something that's not an inode. */
	if (rp->rec_magic != SLOS_RMAGIC) {
	    DBUG("Magic not good\n");
	    free(rp, M_SLOS);
	    return EINVAL;
	}
	 
	*rpp = rp;

	return 0;
}

/* Write a record back to the OSD. */
static int
slos_recdwrite(struct slos *slos, struct slos_record *rp)
{
	if (rp->rec_magic != SLOS_RMAGIC)
	    return EINVAL;

	/* Write bnode to the disk. */
	return slos_writeblk(slos, rp->rec_blkno, rp); 
}

int 
slos_rcreate(struct slos_node *vp, uint64_t rtype, uint64_t *rnop)
{   
	struct slos_diskptr recordptr;
	struct slos_diskptr dataptr = DISKPTR_NULL;
	struct slos_record *rp = NULL;
	struct bnode *bnode = NULL;
	struct slos *slos = vp->sn_slos;
	int error;
	uint64_t rno;

	KASSERT(slos != NULL, ("Slos is null"));

	error = slos_lastrno(vp, &rno);
	if (error == EIO) {
	    return error;
	} else if (error == EINVAL) {
	    rno = 0;
	}

	DBUG("Last record found %lu\n", rno);
	/* Lock the inode. */
	mtx_lock(&vp->sn_mtx);

	/* Get space on disk for the record itself. */
	recordptr = slos_alloc(slos->slos_alloc, 1);
	if (recordptr.offset == 0) {
	    error = ENOSPC;
	    goto error;
	}
	
	/* Get space for the data offset btree root. */
	/* XXX
	 * We can defer this can't we? We shouldn't allocate for an empty btree,
	 * we should make the create as fast as possible, as rcreates are going to happen,
	 * without data every being written to the record.  Another option allocate once,
	 * but of size 2, and have dataptr and record ptr share the allocation */
	dataptr = slos_alloc(slos->slos_alloc, 1);
	if (dataptr.offset == 0) {
	    error = ENOSPC;
	    goto error;
	}

	/* Create the on-disk representation. */
	rp = malloc(slos->slos_sb->sb_bsize, M_SLOS, M_WAITOK | M_ZERO);
	rp->rec_type = rtype;
	rp->rec_length = 0;
	rp->rec_size = 0;
	rp->rec_magic = SLOS_RMAGIC;
	rp->rec_blkno = recordptr.offset;
	rp->rec_data = dataptr;
	rp->rec_num = rno + 1;
	/* Create the root of the data btree. */
	bnode = bnode_alloc(slos, dataptr.offset, 
		sizeof(struct slos_recentry), BNODE_EXTERNAL);
	error = bnode_write(slos, bnode);
	if (error != 0)
	    goto error;

	/* Write the record out to the disk. */
	error = slos_recdwrite(slos, rp);
	if (error != 0)
	    goto error;

	/* Add the record to the records btree. */
	error = btree_insert(vp->sn_records, rp->rec_num, &DISKPTR_BLOCK(rp->rec_blkno));
	if (error != 0) {
	    goto error;
	}

	/* 
	 * Export the updated inode (this includes possibly 
	 * updating the root of the records btree if needed). 
	 */
	error = slos_vpexport(slos, vp);
	if (error != 0) {
	    printf("Could not export vp with error %d\n", error);
	    goto error;
	}

	/* Export the record number to the caller. */
	*rnop = rp->rec_num;
	
	/* Everything went well, clean up the btree. */
	btree_discardelem(vp->sn_records);

	bnode_free(bnode);
	free(rp, M_SLOS);
	mtx_unlock(&vp->sn_mtx);

	return 0;

error:
	btree_keepelem(vp->sn_records);

	bnode_free(bnode);
	free(rp, M_SLOS);

	slos_free(slos->slos_alloc, dataptr);
	slos_free(slos->slos_alloc, recordptr);

	mtx_unlock(&vp->sn_mtx);

	return error;
}

/* Pass the record, we assume the caller will free record entry */
int 
slos_rremove(struct slos_node *vp, struct slos_record *record)
{
	uint64_t key;
	struct slos_recentry val;

	struct slos *slos = vp->sn_slos;
	struct slos_blkalloc *alloc = slos->slos_alloc;
	struct btree *rec_btree = btree_init(slos, record->rec_data.offset, ALLOCMAIN);
	int error;

	/* Free each piece of data in the data btree ondisk*/
	BTREE_KEY_FOREACH(rec_btree, key, val) {
	    slos_free(alloc, val.diskptr);
	    error = btree_delete(rec_btree, key);
	    if (error) {
		DBUG("Problem deleting key value pair in vnode %lu, record %lu, %lu", 
			vp->sn_pid, record->rec_num, key);
		break;
	    }
	}

	if (error) {
	    DBUG("Error removing data from tree\n");
	    return (error);
	}

	/* Remove the record from vnode */
	error = btree_delete(vp->sn_records, record->rec_num);
	if (error) {
	    panic("We should have this key\n");
	    return (error);
	}

	/* Free the btree itself ondisk and memory */
	slos_free(alloc, record->rec_data);
	btree_destroy(rec_btree);
	/* Free the record block itself */
	slos_free(alloc, DISKPTR_BLOCK(record->rec_blkno));
	
	return error;
}

int 
slos_rread(struct slos_node *vp, uint64_t rno, struct uio *auio)
{
	struct slos_recentry preventry, nextentry;
	uint64_t prevoff, nextoff, startoff;
	struct slos_diskptr recordptr;
	uint64_t oldresid, oldoff;
	uint64_t holesize;
	uint64_t size_read;
	int error;

	struct slos_record *rp = NULL;
	struct btree *data = NULL;
	struct slos *slos = vp->sn_slos;

	mtx_lock(&vp->sn_mtx);

	/* Get the record we want from the inode's btree. */
	error = btree_search(vp->sn_records, rno, &recordptr);
	if (error != 0)
	    goto error;

	/* Get the actual record data from the slos-> */
	error = slos_recdread(slos, recordptr.offset, &rp);
	if (error != 0) {
	    goto error;
	}

	/* Error out if we are reading past the end of the file. */
	if (auio->uio_offset >= rp->rec_length) {
	    error = EINVAL;
	    goto error;
	}

	/* Create the in-memory btree of data offsets. */
	data = btree_init(slos, rp->rec_data.offset, ALLOCMAIN);

	/* 
	 * Keep reading until we either reach the end of the 
	 * record, or completely fill up the buffer.
	 */
	while (auio->uio_resid > 0 && auio->uio_offset < rp->rec_length) {
	    /* Get the starting block and the offset within it. */

	    prevoff = auio->uio_offset;

	    /* Find the start of the offset we're going to read from. */
	    error = btree_keymin(data, &prevoff, &preventry);
	    if (error != 0 && error != EINVAL)
		goto error;

	    /* 
	     * If we have nothing to our left, the
	     * beginning of the file has a hole.
	     * Find how large it is.
	     *
	     */
	    if (error == EINVAL) {
		/* This _has_ to exist, since otherwise the size would be 0. */
		error = btree_first(data, &prevoff, &preventry);
		if (error != 0)
		    goto error;

		/* 
		 * The hole is as large as the offset of the first extent. 
		 * We start reading from uio_offset.
		 */
		holesize = prevoff - auio->uio_offset;
		if (holesize > auio->uio_resid)
		    holesize = auio->uio_resid;


		/* Fill the UIO with zeros. */
		error = slos_uiozero(auio, holesize);
		if (error != 0)
		    goto error;


	    } else if (prevoff + preventry.len <= auio->uio_offset) {
		/* 
		 * We are reading into a hole, fill the buffer with zeros
		 * until we reach the next real extent of data. An extent
		 * to the right exists, since we have checked we are
		 * not past the end of the file.
		 */
		nextoff = auio->uio_offset;
		error = btree_keymax(data, &nextoff, &nextentry);
		if (error != 0)
		    goto error;

		/*
		 * The part of the hole we can read start from
		 * uio_offset up to nextoff.
		 */
		holesize = nextoff - auio->uio_offset;
		if (holesize > auio->uio_resid)
		    holesize = auio->uio_resid;
		    
		/* Fill the UIO with zeros. */
		error = slos_uiozero(auio, holesize);
		if (error != 0)
		    goto error;

	    } else {
		/* 
		 * Otherwise we are reading real data. We use
		 * the disk pointer we got from btree_keymin().
		 */

		/* Save the file offset and resid for later. */
		oldoff = auio->uio_offset;
		oldresid = auio->uio_resid;

		/* Fixup the UIO to read exactly the valid data. */
		if (auio->uio_resid > 
			(prevoff + preventry.len) - auio->uio_offset)
		    auio->uio_resid = 
			(prevoff + preventry.len) - auio->uio_offset;
		auio->uio_offset = (auio->uio_offset - prevoff) + preventry.offset;
		startoff = auio->uio_offset;

		/* Do the UIO itself, then revert the UIO resid and offset. */
		error = slos_read(slos, &preventry.diskptr, auio);
		if (error != 0)
		    goto error;

		size_read = auio->uio_offset - startoff;
		auio->uio_resid = oldresid - size_read;
		auio->uio_offset = oldoff + size_read;

	    }
	}

	free(rp, M_SLOS);
	mtx_unlock(&vp->sn_mtx);

	btree_destroy(data);

	return 0;
error:
	DBUG("Error reading record %lu, %d\n", rno, error);
	if (data != NULL)
	    btree_destroy(data);

	free(rp, M_SLOS);
	mtx_unlock(&vp->sn_mtx);

	return error;
}

int 
slos_rwrite(struct slos_node *vp, uint64_t rno, struct uio *auio)
{
	struct slos_recentry leftentry, rightentry, curentry;
	uint64_t newoff, leftoff, rightoff, curoff;
	struct slos_diskptr recordptr;
	uint64_t blksize, xfersize;
	uint64_t oldresid;

	struct slos_diskptr newdata = DISKPTR_NULL;
	struct slos_recentry newentry = {DISKPTR_NULL, 0, 0};
	struct slos_record *rp = NULL;
	struct slos *slos = vp->sn_slos;
	struct btree *data = NULL;
	int error = 0;

	/* XXX HACK for special case. */
	struct uio *hackuio = cloneuio(auio);

	mtx_lock(&vp->sn_mtx);

	/* Get the record we want from the inode's btree. */
	error = btree_search(vp->sn_records, rno, &recordptr);
	if (error != 0)
	    goto error;

	/* Get the actual record data from the slos-> */
	error = slos_recdread(slos, recordptr.offset, &rp);
	if (error != 0)
	    goto error;

	/* Create the in-memory btree of data offsets. */
	data = btree_init(slos, rp->rec_data.offset, ALLOCMAIN);

	/* 
	 * While we haven't written everything yet, allocate enough
	 * space to write the data, write it in, then insert it to
	 * the data btree, overwriting any existing data.
	 */
	while (auio->uio_resid > 0) {
	    /* Find the size of the new data in blocks. */
	    blksize = auio->uio_resid / slos->slos_sb->sb_bsize;
	    if ((auio->uio_resid % slos->slos_sb->sb_bsize) != 0)
		blksize += 1;

	    /* Get as large an extent as possible. */
	    newoff = auio->uio_offset;
	    newdata = slos_alloc(slos->slos_alloc, blksize);
	    if (newdata.offset == 0) {
		error = ENOSPC;
		goto error;
	    }

	    /* 
	     * Fill up the newly allocated extent 
	     * with as much data as possible.
	     */
	    xfersize = newdata.size * slos->slos_sb->sb_bsize;
	    if (xfersize > auio->uio_resid)
		xfersize = auio->uio_resid;


	    /* 
	     * Fix up the offset to be in relation 
	     * to the extent, not the file. Also
	     * make sure we write exactly as much
	     * data as we have/can fit.
	     */
	    oldresid = auio->uio_resid;

	    auio->uio_offset = 0;
	    auio->uio_resid = xfersize;

	    do {
		error = slos_write(slos, &newdata, auio);
		if (error != 0)
		    goto error;

	    } while (auio->uio_resid > 0);

	    /* The new offset is the old one plus the bytes written. */
	    auio->uio_offset = newoff + xfersize;

	    /* Create the new entry for the data btree. */
	    newentry = (struct slos_recentry) {newdata, 0, xfersize};
	    
	    /* 
	     * Now that we filled up the extent, insert it 
	     * to the btree, overwriting any existing data. 
	     * There are various overlaps that can happen:
	     *
	     * - Partial overlap to the left.
	     * - Partial overlap to the right.
	     * - The new extent is completely inside an old one.
	     * - An old extent is completely inside the new one.
	     */

	    /* 
	     * First find the immediate left neighbor. 
	     * From there we can see if we partially
	     * or totally overlap.
	     */
	    leftoff = newoff;
	    error = btree_keymin(data, &leftoff, &leftentry);
	    if (error != EINVAL && error != 0)
		goto error;
		
	    /* Check if we actually have a left neighbour. */
	    if (error == 0) {
		/* Check if we are completely inside the left neighbor.*/
		if ((leftoff + leftentry.len) >= (newoff + newentry.len)) {
		    /* 
		     * XXX Ridiculously special case: The changes
		     * are so small that they span less than a page.
		     * That means that we _cannot_ split the old 
		     * extent in two, since both parts would need
		     * to have the same block. In this case, we 
		     * just overwrite the old block and don't 
		     * add a new extent. Since we only span
		     * one block, the operation is atomic,
		     * and everything works out!
		     */

		    /* XXX Otherwise Split the old extent in two. */

		    /* XXX Enter the new extent. */

		    /* 
		     * XXX HACK: The above sequence of events is
		     * correct, but for now we just overwrite
		     * the old data, let's get on with it.
		     */

		    /* Fixup uio_offset and uio_resid. */
		    hackuio->uio_offset = leftentry.offset + (newoff - leftoff);
		    hackuio->uio_resid = newentry.len;



		    /* Hacky write */
		    error = slos_write(slos, &leftentry.diskptr, hackuio);
		    if (error != 0)
			goto error;

		    /* auio has already been fixed up, because we wrote into newentry. */
		    /* We never used newentry, free */
		    slos_free(slos->slos_alloc, newdata);

		    /* 
		     * We were contained in one entry, so 
		     * no other overlaps are possible. 
		     */
		    continue;
		} else if ((leftoff + leftentry.len) > newoff) {
		    /* Othewise check whether we partially overlap. */

		    /* 
		    * Otherwise the end overlaps with the new start.
		    * Just truncate the old entry by reducing its
		    * end pointer. We could free any blocks possibly
		    * not in use anymore, but it isn't necessary.
		    */
		    /* The new end is such that the length is newoff - leftoff. */
		    leftentry.len = newoff - leftoff;
		    error = btree_overwrite(data, leftoff, &leftentry, NULL);
		    if (error != 0)
			goto error;

		}
	    }

	    /*
	     * Check for any existing extents that we completely
	     * engulf. These can be simply removed. Since we
	     * have also truncated any possible partially
	     * overlapping right neighbors, we can assume
	     * that if an extent's offset is in the new 
	     * extent, the whole old extent is inside 
	     * the new one.
	     */
	    curoff = newoff;
	    for (;;) {

		error = btree_keymax(data, &curoff, &curentry);
		if (error != 0 && error != EINVAL)
		    goto error;

		/* We reached the end of the file. */
		if (error == EINVAL)
		    break;
		
		/* 
		 * If the entry we found is not completely
		 * in the new extent, we're done here.
		 */
		if ((curoff + curentry.len) > (newoff + newentry.len))
		    break;

		/* While curoff is in the new entry, delete its extent. */
		error = btree_delete(data, curoff);
		if (error != 0)
		    goto error;

		/* 
		 * XXX Normally we would check to see whether the elements
		 * should be GC'ed (they might still be valid in a
		 * clone of this record). However, because the GC is not
		 * yet operational, to save as much space as possible 
		 * we delete them. In the future, this code will be 
		 * removed and freeing memory will be done by the GC.
		 */
		slos_free(slos->slos_alloc, curentry.diskptr);
		rp->rec_size -= curentry.diskptr.size;

	    } 

	    /*
	     * Now find the immediate right neighbor.
	     * Since we checked for the case of total
	     * overlap, we only check for partial
	     * overlap here.
	     */
	    rightoff = newoff;
	    error = btree_keymax(data, &rightoff, &rightentry);
	    if (error != EINVAL && error != 0)
		goto error;

	    if (error == 0 && (newoff + newentry.len > rightoff)) {
		/* 
		 * Similar to an overlap to the left, we 
		 * truncate the entry by modifying its
		 * start pointer. There is no reason
		 * to free any unused blocks.
		 *
		 * Here, we also need to remove and 
		 * reinsert the entry into the btree,
		 * since we are changing the starting 
		 * offset.
		 */
		error = btree_delete(data, rightoff);
		if (error != 0)
		    goto error;


		/* 
		 * Move the start by as many bytes as the
		 * end of the new extent is larger than
		 * the start of the right extent.
		 */
		rightentry.offset += (newoff + newentry.len) - rightoff; 
		rightentry.len -= (newoff + newentry.len) - rightoff; 

		/* 
		 * The new right offset is exactly 
		 * at the end of the new extent.
		 */
		rightoff = newoff + newentry.len;

		/* Insert the modified entry. */
		error = btree_insert(data, rightoff, &rightentry);
		if (error != 0) {
		    goto error;
		}
	    }


	    /* Now that we've made room insert the new extent. */
	    error = btree_insert(data, newoff, &newentry);
	    if (error != 0)
		goto error;
	    newdata = DISKPTR_NULL;

	    /* Update the record file and block size. */
	    if (newoff + newentry.len > rp->rec_length)
		rp->rec_length = newoff + newentry.len;

	    rp->rec_size += newentry.diskptr.size;

	}

	/* If the data root btree changed, update the pointer. */
	if (data->root != rp->rec_data.offset)
	    rp->rec_data.offset = data->root;

	error = slos_recdwrite(slos, rp);
	if (error != 0)
	    goto error;

	btree_discardelem(data);
	btree_destroy(data);

	free(rp, M_SLOS);

	mtx_unlock(&vp->sn_mtx);

	/* XXX HACK */
	free(hackuio, M_IOV);

	return 0;

error:
	
	btree_discardelem(data);
	slos_free(slos->slos_alloc, newdata);

	if (data != NULL) {
	    btree_keepelem(data);
	    btree_destroy(data);
	}

	free(rp, M_SLOS);
	mtx_unlock(&vp->sn_mtx);

	/* XXX HACK */
	free(hackuio, M_IOV);

	return error;
}

/* Assuming vnode must will be locked on entry into this function */
int 
slos_getrec(struct slos_node *vp, uint64_t rno, struct slos_record **rec)
{
	struct slos_diskptr recordptr;
	struct slos_record *rp;
	int error;

	/* Get the record we want from the inode's btree. */
	error = btree_search(vp->sn_records, rno, &recordptr);
	if (error) 
	   return (error);

	/* Get the actual record data from the slos-> */
	error = slos_recdread(vp->sn_slos, recordptr.offset, &rp);
	if (error) 
	   return (error);
	
	*rec = rp;

	return (0);
}

/* 
 * Seek the record, returning the position of a boundary between a hole 
 * and an extent that is as close to the offset as possible and
 * satisfies the conditions specified by the flags argument. The 
 * seek points to the closest _beginning_ of an extent; this 
 * matters when considering that offset may be in the middle
 * of an extent.
 */
int
slos_rseek(struct slos_node *vp, uint64_t rno, uint64_t offset, 
	int flags, uint64_t *seekoffp, uint64_t *seeklenp)
{
	struct slos_recentry preventry, nextentry;
	struct slos_record *rp = NULL;
	struct slos_diskptr recordptr;
	struct slos *slos = vp->sn_slos;
	uint64_t prevoff, nextoff;
	struct btree *data = NULL;
	int error = 0;

	/* XXX We don't implement seeking to the left right now. */
	/* If we don't want to seek left, we implicitly want to seek right. */
	if ((flags & SREC_SEEKLEFT) != 0)
	    return EINVAL;
	else
	    flags |= SREC_SEEKRIGHT;

	mtx_lock(&vp->sn_mtx);

	/* Get the record we want from the inode's btree. */
	error = btree_search(vp->sn_records, rno, &recordptr);
	if (error != 0)
	    goto out;

	/* Get the actual record data from the slos-> */
	error = slos_recdread(slos, recordptr.offset, &rp);
	if (error != 0)
	    goto out;

	/* Create the in-memory btree of data offsets. */
	data = btree_init(slos, rp->rec_data.offset, ALLOCMAIN);

	if ((flags & SREC_SEEKHOLE) != 0) {
	    /* 
	     * If we're seeking a hole, go towards the end
	     * until we find two consecutive extents that 
	     * are not contiguous.
	     */
	    prevoff = offset;
	    error = btree_keymax(data, &prevoff, &preventry);
	    if (error == EINVAL) {
		/* We reached EOF, return the size of the record. */
		*seekoffp = rp->rec_length;
		*seeklenp = SREC_SEEKEOF;
		error = 0;
		goto out;

	    } else if (error != 0) { 
		/* True error, abort. */
		goto out;
	    }

	    do {
		/* 
		 * Try to search for an extent that 
		 * starts right at prevoff's end.
		 */
		nextoff = prevoff + preventry.len;
		error = btree_search(data, nextoff, &nextentry);
		if (error == EINVAL) {
		    /* 
		     * If there isn't, there is a hole starting from that offset. 
		     * This includes the case where we have reached the end of the
		     * file, in which case we make sure we point right at the end.
		     */
		    if (nextoff > rp->rec_length) {
			*seekoffp = rp->rec_length;
			*seeklenp = SREC_SEEKEOF;
		    } else {
			*seekoffp = nextoff;
			*seeklenp = nextentry.len;
		    }

		    error = 0;
		    goto out;

		} else if (error != 0) {
		    /* True error, abort. */
		    goto out;
		}

		/* 
		 * Otherwise there's an extent in there.
		 * Try to find its own right neighbor in
		 * the next iteration.
		 */
		prevoff = nextoff;
		preventry = nextentry;

	    } while (prevoff < rp->rec_length);

	    /* If we reached here, we didn't find anything. */
	    *seekoffp = rp->rec_length;
	    *seeklenp = SREC_SEEKEOF;
	    
	} else {

	    /* Otherwise we're seeking data. Just do a keymax. */
	    prevoff = offset;
	    error = btree_keymax(data, &prevoff, &preventry);
	    if (error == EINVAL) {
		/* We reached EOF, return the size of the record. */
		*seekoffp = rp->rec_length;
		*seeklenp = SREC_SEEKEOF;
		error = 0;
		goto out;

	    } else if (error != 0) { 
		/* True error, abort. */
		goto out;
	    }

	    /* Else we found a next offset. Return it. */
	    *seekoffp = prevoff;
	    *seeklenp = preventry.len;

	}
	    
out:

	free(rp, M_SLOS);
	mtx_unlock(&vp->sn_mtx);

	if (data != NULL)
	    btree_destroy(data);

	return error;
}

/* 
 * Get information for a specific record in the vnode. 
 * Right now these are the size and type.
 */
int
slos_rstat(struct slos_node *vp, uint64_t rno, struct slos_rstat *stat)
{
	struct slos_diskptr ptr;
	struct slos_record *rp;
	int error;

	struct slos *slos = vp->sn_slos;

	mtx_lock(&vp->sn_mtx);

	error = btree_search(vp->sn_records, rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    return error;
	}

	error = slos_recdread(slos, ptr.offset, &rp);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    return 0;
	}

	mtx_unlock(&vp->sn_mtx);

	stat->type = rp->rec_type;
	stat->len = rp->rec_length;

	free(rp, M_SLOS);

	return 0;
}

/* 
 * Helpers for iterating the records btree. We can have the following movements:
 * - Go to first record in the btree.
 * - Go to last record in the btree.
 * - Go to the next record in the btree from a specific offset.
 * - Go to the previous record in the btree from a specific offset.
 *
 * From these basic movements we can compose more complicated ones, like for
 * example finding the first or last record that has a specific type.
 */

int 
slos_prevrec(struct slos_node *vp, uint64_t rno, struct slos_record **record)
{
	struct slos_diskptr ptr;
	struct slos_record *rp;
	struct slos *slos = vp->sn_slos;
	int error;

	mtx_lock(&vp->sn_mtx);

	if (rno == 0)
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return EINVAL;

	rno = rno - 1;
	error = btree_keymin(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return error;
	}

	error = slos_recdread(slos, ptr.offset, &rp);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return error;
	}

	mtx_unlock(&vp->sn_mtx);
	*record = rp;

	return (0);
}

int
slos_nextrec(struct slos_node *vp, uint64_t rno, struct slos_record ** record)
{
	struct slos_diskptr ptr;
	struct slos_record *rp;
	int error;

	struct slos *slos = vp->sn_slos;

	mtx_lock(&vp->sn_mtx);

	rno = rno + 1;
	error = btree_keymax(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return error;
	}

	error = slos_recdread(slos, ptr.offset, &rp);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return error;
	}

	mtx_unlock(&vp->sn_mtx);
	*record = rp;

	return (0);
}

int
slos_firstrec(struct slos_node *vp, struct slos_record ** record)
{
	struct slos_diskptr ptr;
	struct slos_record *rp;
	struct slos *slos = vp->sn_slos;
	uint64_t rno;
	int error;

	mtx_lock(&vp->sn_mtx);

	rno = 0;
	error = btree_keymax(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return (EINVAL);
	}

	error = slos_recdread(slos, ptr.offset, &rp);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    *record = NULL;
	    return (EIO);
	}

	mtx_unlock(&vp->sn_mtx);
	*record = rp;

	return (0);
}

static struct slos_record *
slos_lastrec(struct slos_node *vp)
{
	struct slos_diskptr ptr;
	struct slos_record *rp;
	uint64_t rno;
	int error;

	struct slos *slos = vp->sn_slos;

	mtx_lock(&vp->sn_mtx);

	error = slos_lastrno(vp, &rno);
	if (error) {
	    mtx_lock(&vp->sn_mtx);

	    return (NULL);
	}

	error = btree_keymin(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);

	    return (NULL);
	}

	error = slos_recdread(slos, ptr.offset, &rp);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);

	    return (NULL);
	}

	mtx_unlock(&vp->sn_mtx);

	return (rp);
}

int
slos_firstrno(struct slos_node *vp, uint64_t *rnop)
{
	struct slos_diskptr ptr;
	uint64_t rno;
	int error;

	mtx_lock(&vp->sn_mtx);

	rno = 0;
	error = btree_keymax(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    return error;
	}

	*rnop = rno;

	mtx_unlock(&vp->sn_mtx);

	return 0;
}

int
slos_lastrno(struct slos_node *vp, uint64_t *rnop)
{
	struct slos_diskptr ptr;
	uint64_t rno;
	int error;

	mtx_lock(&vp->sn_mtx);

	error = btree_last(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    return error;
	}

	*rnop = rno;

	mtx_unlock(&vp->sn_mtx);

	return 0;
}

int
slos_prevrno(struct slos_node *vp, uint64_t *rnop)
{
	struct slos_diskptr ptr;
	uint64_t rno;
	int error;

	if (*rnop == 0)
	    return EINVAL;

	mtx_lock(&vp->sn_mtx);

	rno = *rnop - 1;
	error = btree_keymin(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    return error;
	}

	*rnop = rno;

	mtx_unlock(&vp->sn_mtx);

	return 0;
}

int
slos_nextrno(struct slos_node *vp, uint64_t *rnop)
{
	struct slos_diskptr ptr;
	uint64_t rno;
	int error;

	mtx_lock(&vp->sn_mtx);

	rno = *rnop + 1;
	error = btree_keymax(vp->sn_records, &rno, &ptr);
	if (error != 0) {
	    mtx_unlock(&vp->sn_mtx);
	    return error;
	}

	mtx_unlock(&vp->sn_mtx);
	*rnop = rno;

	return 0;
}

int
slos_firstrno_typed(struct slos_node *vp, uint64_t rtype, uint64_t *rnop)
{
	struct slos_record *rp = NULL;	
	uint64_t rno;
	int error = 0;

	mtx_lock(&vp->sn_mtx);

	error = slos_firstrec(vp, &rp);
	if (error) {
	    return (error);
	}
	while (rp != NULL && rp->rec_type != rtype && !error) {
	    rno = rp->rec_num;
	    free(rp, M_SLOS);
	    error = slos_nextrec(vp, rno, &rp);
	}

	if (error) {
	    return (error);
	}
	
	if (rp != NULL && rp->rec_type == rtype) {
	    *rnop = rp->rec_num;
	    free(rp, M_SLOS);
	    mtx_unlock(&vp->sn_mtx);

	    return (0);
	}

	mtx_unlock(&vp->sn_mtx);

	return (EINVAL);
}

int
slos_lastrno_typed(struct slos_node *vp, uint64_t rtype, uint64_t *rnop)
{
	struct slos_record *rp = NULL;	
	uint64_t rno;
	int error = 0;

	mtx_lock(&vp->sn_mtx);

	rp = slos_lastrec(vp);
	while (rp != NULL && rp->rec_type != rtype && !error) {
	    rno = rp->rec_num;
	    free(rp, M_SLOS);
	    error = slos_prevrec(vp, rno, &rp);
	}
	if (error) {
	    return (error);
	}

	if (rp != NULL && rp->rec_type == rtype) {
	    *rnop = rp->rec_num;
	    free(rp, M_SLOS);
	    mtx_unlock(&vp->sn_mtx);

	    return 0;
	}

	mtx_unlock(&vp->sn_mtx);

	return EINVAL;
}

#ifdef SLOS_TESTS

#define TEXTSIZE    (4096)   /* The size of the data to be written */
#define RECPID	    (1024)	    /* The PID of the inode */
#define TESTREC	    (0xcafe)	    /* Arbitrary record type for testing */
#define ITERATIONS  (10000)	    /* Number of test iterations */
#define	OPSIZE	    (8)	    /* Maximum operation size */
#define POISON	    ('^')	    /* Poison value */

#define PREAD	    80		    /* Probability weight of the read operation */
#define PWRITE	    80		    /* Probability weight of the write operation */
#define PSEEK	    80		    /* Probability weight of the seek operation */

int
slos_test_record(void)
{
	struct slos_node *vp = NULL;
	uint64_t holesize, holeoff;
	uint64_t seekoff, seeklen;
	uint64_t offset, len;
	char *result = NULL;
	int ino_created = 0;
	int iter, operation;
	char *text = NULL;
	struct iovec aiov;
	struct uio auio;
	int seek_hole;
	int error = 0;
	uint64_t rno;
	int i, diffs;
	int flags;

	/* Get a zeroed buffer as initial input. */
	text = malloc(TEXTSIZE, M_SLOS, M_WAITOK | M_ZERO);
	result = malloc(OPSIZE, M_SLOS, M_WAITOK | M_ZERO);
	
	/* Set up the inode. */
	error = slos_icreate(&slos, RECPID, VREG);
	if (error != 0)
	    goto error;

	ino_created = 1;

	vp = slos_iopen(&slos, RECPID);
	if (vp == NULL) {
	    error = EINVAL;
	    goto error;
	}


	/* Create a record for the inode. */
	error = slos_rcreate(vp, TESTREC, &rno);
	if (error != 0)
	    goto error;


	/* 
	 * Write exactly one character past the
	 * end of the area we are going to use
	 * for the test. The net result is the
	 * creation of a hole of TEXTSIZE bytes.
	 * We arbitrarily use the buffer as input.
	 */
	aiov.iov_base = text;
	aiov.iov_len = 1;
	slos_uioinit(&auio, TEXTSIZE, UIO_WRITE, &aiov, 1);


	error = slos_rwrite(vp, rno, &auio);
	if (error != 0)
	    goto error;


	/* 
	 * Write a bit past TEXTSIZE to force it
	 * to a hole where we are going to write. 
	 */


	/* Iterate: For each iteration, either read
	 * or write an area. We pick the offset and
	 * the size randomly; that's ok, because our
	 * input is  zeroes, and if we haven't written
	 * to the record, the it has holes that cause
	 * 0s to be read - just like in the uninitialized
	 * parts of our buffer. We can force the file to
	 * be of a certain size by writing a bit past the
	 * end of the buffer.
	 */
	for (iter = 0; iter < ITERATIONS; iter++) {

	    /* 
	     * Poison the read buffer (both reads 
	     * and writes read from the record).
	     */

	    operation = random() % (PREAD + PWRITE + PSEEK);
	    /* 
	     * Get a random offset and size 
	     * for the operation. 
	     */
	    offset = random() % TEXTSIZE;
	    len = random() % OPSIZE;
	    if (offset + len > TEXTSIZE)
		len = TEXTSIZE - offset;

	    if (operation < PREAD) {

		memset(result, POISON, OPSIZE);

		/* Set up the read UIO. */
		aiov.iov_base = result;
		aiov.iov_len = len;
		slos_uioinit(&auio, offset, UIO_READ, &aiov, 1);

		error = slos_rread(vp, rno, &auio);
		if (error != 0) {
		    goto error;
		}

		if (memcmp(result, &text[offset], len) != 0) {
		    printf("ERROR: slos_rread() returned different data than \
			    what was written by slos_rwrite()\n");
		    printf("=========DATA RECEIVED=========\n");
		    printf("(len %ld) %.*s\n", len, (int) len, result);
		    printf("=========DATA EXPECTED=========\n");
		    printf("(len %ld) %.*s\n", len, (int) len, &text[offset]);
		    for (i = 0; result[i] == text[offset + i]; i++)
			;
		    printf("Difference at offset %d (%d vs %d)\n", 
			    i, result[i], text[offset + i]); 
		    
		    diffs = 1;
		    for (; i < len; i++) {
			if (result[i] != text[offset + i])
			    diffs += 1;
		    }
		    printf("Total differences: %d\n", diffs);
		    error = EINVAL;

		    goto error;
		}

	    } else if (operation < PREAD + PWRITE) {
		/* 
		 * Fill up the output and the total 
		 * buffers with random characters.
		 */
		for (i = 0; i <  len; i++) {
		    result[i] = 'a' + (random() % ('z' - 'a'));
		    text[offset + i] = result[i];
		}

		/* Set up the write UIO. */
		aiov.iov_base = &text[offset];
		aiov.iov_len = len;
		slos_uioinit(&auio, offset, UIO_WRITE, &aiov, 1);

		error = slos_rwrite(vp, rno, &auio);
		if (error != 0) {
		    printf("ERROR: slos_rwrite() failed with %d\n", error);
		    goto error;
		}

		/* Read the written data back up. */
		aiov.iov_base = result;
		aiov.iov_len = len;
		slos_uioinit(&auio, offset, UIO_READ, &aiov, 1);
		error = slos_rread(vp, rno, &auio);
		if (error != 0) {
		    printf("ERROR: slos_rread() (after write) failed with %d\n", error);
		    goto error;
		}

		if (memcmp(result, &text[offset], len) != 0) {
		    printf("ERROR: slos_rread() (after write) returned different \
			    data than what was written by slos_rwrite()\n");
		    printf("=========DATA RECEIVED=========\n");
		    printf("%.*s\n", (int) len, result);
		    printf("=========DATA EXPECTED=========\n");
		    printf("%.*s\n", (int) len, &text[offset]);
		    for (i = 0; result[i] == text[offset + i]; i++)
			;
		    printf("Difference at offset %d (%d vs %d)\n", 
			    i, result[i], text[offset + i]); 
		    
		    diffs = 1;
		    for (; i < len; i++) {
			if (result[i] != text[offset + i])
			    diffs += 1;
		    }
		    printf("Total differences: %d\n", diffs);

		    error = EINVAL;

		    goto error;
		}


	    } else {
		/* 50% chance we'll look for holes, 50% we'll look for extents. */
		/* XXX We don't care about holes right now. */
		//seek_hole = ((random() % 2) != 0);
		seek_hole = 0;
		flags = SREC_SEEKRIGHT | (seek_hole ? SREC_SEEKHOLE : 0);

		/* Seek the extent/hole. */
		error = slos_rseek(vp, rno, offset, flags, &seekoff, &seeklen);
		if (error != 0) {
		    printf("ERROR: Seeking %s at offset %lu failed with %d\n", 
			    seek_hole ? "hole" : "data", offset, error);
		    break;
		}

		/* XXX SREC_SEEKHOLE is buggy, fix later. */
		if (seek_hole) {
		    /* 
		     * If the hole we were seeking was at the beginning 
		     * of the file, look if we are returning the 0 offset.
		     */
		    if (offset == 0 && text[0] == '\0') {
			holeoff = 0;
		    } else {
			for (holeoff = offset; holeoff < TEXTSIZE; holeoff++) {
			    if (text[holeoff - 1] != '\0' && text[holeoff] == '\0')
				break;
			}
		    }

		    /* If there isn't a hole, make sure we didn't find one. */
		    if (holeoff == TEXTSIZE) {
			/* If the seek failed, the length has a special value. */
			if (seeklen != SREC_SEEKEOF) {
			    printf("ERROR: Unexpected hole found at (%ld, %ld)\n",
				  seekoff, seeklen);
			    break;
			}

			/* Otherwise everything is fine, go to next iteration. */
			continue;

		    } else {
			/* 
			 * Otherwise we found something, and we need to compare 
			 * the received result with the expected one.
			 */

			/* First find the size of the hole. */
			for (holesize = 0; holeoff + holesize < TEXTSIZE; holesize++) {
			    if (text[holeoff + holesize] != '\0')
				break;
			}

			if ((holeoff != seekoff) || (holesize != seeklen)) {
			    printf("ERROR: Expected to find hole at (%ld, %ld), ", 
				    holeoff, holesize);
			    printf("found one at (%ld, %ld)\n", seekoff, seeklen);
			}
		    }

		} else {
		    /* 
		     * We're seeking an extent. Unfortunately, we _cannot_
		     * compute the expected answer from the flat buffer, since
		     * there can be consecutive extents without a hole separating
		     * them, which in turn means there is no way to find out
		     * how the data is laid out in terms of extents by the buffer.
		     * For this reason, we rely on the test code for the holes
		     * above, and assume that the code for seeking extents is
		     * similarly correct.
		     */
		}
	    }
	}


	slos_iclose(&slos, vp);
	slos_iremove(&slos, RECPID);

	free(result, M_SLOS);
	free(text, M_SLOS);

	return 0;
error:
	if (vp != NULL)
	    slos_iclose(&slos, vp);

	if (ino_created != 0)
	    slos_iremove(&slos, RECPID);

	free(result, M_SLOS);
	free(text, M_SLOS);

	return error;
}

#endif /* SLOS_TESTS */
