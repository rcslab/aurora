#include <sys/types.h>

#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

#include <machine/cpufunc.h>
#include <machine/param.h>
#include <machine/reg.h>
#include <machine/vmparam.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "sls.h"
#include "path.h"
#include "slsmm.h"
#include "sls_data.h"
#include "sls_dump.h"
#include "sls_file.h"
#include "sls_snapshot.h"
#include "sls_osd.h"
#include "sls_mosd.h"

/*
 * Obligatory ASCII art for the OSD:
 *
 * 0		    ---------------------------------
 *		    |		Superblock	    |
 * osd_allocoff	    ---------------------------------
 *		    |				    |
 *		    |		Bitmap		    |
 *		    |	(maps the whole disk,	    |
 *		    |    including superblock	    | 
 *		    |	    and inos)		    |
 * osd_inodeoff	    ---------------------------------
 *		    |				    |
 *		    |		Inode Table	    |
 *		    |				    |
 * osd_firsblk	    ---------------------------------
 *		    |				    |	
 *		    |		Data Blocks	    |
 *		    |				    |
 * osd_numblks *    ---------------------------------
 * osd_bsize
 *
 */

/* Mask from trailing zeros */ 
static uint64_t 
blsmsk(uint64_t x)
{
    return ~x & (x - 1);
}

/*
 * Loop in the area [start, end] starting from pos,
 * trying to find a free bit. Return the on-disk
 * position of the block found. Size is both input and output.
 */
static uint64_t
mbmp_getbit(struct osd_mbmp *mbmp, uint64_t start, uint64_t end, 
	uint64_t *pos, uint64_t *size)
{
    uint64_t indx;
    //uint64_t word;
    uint64_t retstart;
    //uint64_t firstunset, lastunset;
    //uint64_t mask, overshoot;
    //uint64_t cursize;
    uint64_t original_indx;

    indx = *pos;
    while (mbmp->mbmp_bmp[indx] == UINT64_MAX) {
	indx = start + ((indx + 1) % (end - start));

	if (indx == *pos) {
	    printf("Error: OSD Full.\n");
	    /* 
	     * End is not actually in the 
	     * area we are searching, so
	     * it's always invalid.
	     */
	    return end * sizeof(uint64_t) * 8;
	}
    }

    original_indx = indx;
    retstart = ffsll(~(mbmp->mbmp_bmp[indx])) - 1;

#if 0
    do  {
	word = mbmp->mbmp_bmp[indx];

	/* Find the first unset bit of the word (counting from the LSB) */
	firstunset = ffsll(~(word));

	/* Create a mask that covers all bits up to the first unset one */
	mask = blsmsk(1 << firstunset);

	/* Find the the last unset bit in a consecutive range with the first unset */
	lastunset = ffsll(~(word & mask));
	cursize = lastunset - firstunset + 1;

	mask = blsmsk(1 << lastunset);
	mbmp->mbmp_bmp[indx++] |= mask;

	/* Keep iterating through the allowed space */
	offset += 1;
	indx = start + (offset % (end - start));
    } while (lastunset == 63 && cursize < *size && indx != *pos);

    /* Roll back last increment in the loop */
    printf("index is: %ld\n", indx);
    //offset -= 1;
    indx = start + (offset % (end - start));
    printf("index now is: %ld\n", indx);

    /* If we overallocated, we did so in the last iteration. We fix that here. */
    if (cursize > *size) {
	overshoot = cursize - *size;
	/* 
	 * Example of what the hell we're doing:
	 * Let's say we overshot by 2:
	 * ------------------------
	 *  1 0 1 0 1 1 1 0 0 0 1 1	(Original word)
	 * ------------------------
	 *  0 0 0 0 0 0 0 1 1 1 1 1	(blmsk(1 << lastunset))
	 * ------------------------
	 *  1 1 1 1 1 1 1 1 1 0 0 0	(~blmsk(1 << (lastunset - overshoot)))
	 * ------------------------
	 *  1 1 1 1 1 1 1 0 0 1 1 1	(~mask)
	 * ------------------------
	*/ 
	mask = blsmsk(1 << lastunset) & (~blsmsk(1 << (lastunset - overshoot)));

	mbmp->mbmp_bmp[indx] &= (~mask);
    }
#endif

    /* XXX TEMP */
    mbmp->mbmp_bmp[indx] |= (1ULL << retstart);
    *size = 1;

    *pos = indx;
    //*size = cursize;

    return original_indx * 64 + retstart;
}


/*
 * Free the bit corresponding to block.
 *
 * XXX Allow for multiblock releases, it's a small step
 * considering we are doing multiblock allocations.
 */
static void
mbmp_freebit(struct osd_mbmp *mbmp, uint64_t block)
{
    uint64_t word, offset; 

    word = block / (sizeof(uint64_t) * 8);
    offset = block % (sizeof(uint64_t) * 8);

    mbmp->mbmp_bmp[word] &= ~(1 << offset);
}

/* Methods related to in-memory inodes. */

/*
 * Allocate a block corresponding to an inode in
 * the disk.
 */
struct osd_mino *
mino_alloc(struct osd_mbmp *mbmp, int flush)
{
    uint64_t block;
    struct osd_mino *mino;
    uint64_t start;
    uint64_t size = 1;
        
    start = mbmp->mbmp_icur;
    block = mbmp_getbit(mbmp, mbmp->mbmp_istart, mbmp->mbmp_bstart, &mbmp->mbmp_icur, &size);
    /* Sign that we failed to allocate due to a full disk */
    if (size != 1) {
	printf("Failed to allocate block in disk\n");
	return NULL;
    }

    if (flush != 0) {
	/* XXX Write the modified part of the bitmap into the OSD */
    }

    mino = malloc(sizeof(*mino), M_SLSMM, M_WAITOK);
    mino->mino_mbmp = mbmp;
    mino->mino_ondisk = block;
    LIST_INIT(&mino->mino_records);
     
    return mino;
}

void
mino_free(struct osd_mino *mino)
{
    mbmp_freebit(mino->mino_mbmp, mino->mino_ondisk);
    //free(mino, M_SLSMM);
}

/* Methods related to in-memory records. */

struct osd_mrec *
mrec_alloc(struct osd_mbmp *mbmp, int flush, int allocbuf)
{
    uint64_t block;
    struct osd_mrec *mrec;
    uint64_t start;
    uint64_t size;
        
    size = 1;
    start = mbmp->mbmp_bcur;
    block = mbmp_getbit(mbmp, mbmp->mbmp_bstart, mbmp->mbmp_size, &start, &size);
    if (size != 1) {
	printf("Warning: Size %ld returned for allocation of size 1\n", size);
    }

    if (flush != 0) {
	/* XXX Write the modified part of the bitmap into the OSD */
    }

    mrec = malloc(sizeof(*mrec), M_SLSMM, M_WAITOK);
    mrec->mrec_size = 1;
    mrec->mrec_ondisk = block;
    mrec->mrec_mbmp = mbmp;
    if (allocbuf != 0) {
	mrec->mrec_buf = malloc(mbmp->mbmp_osd->osd_bsize, M_SLSMM, M_WAITOK);
	mrec->mrec_len = mbmp->mbmp_osd->osd_bsize;
    } else {
	mrec->mrec_len = 0;
    }
    mrec->mrec_allocbuf = allocbuf;
    mrec->mrec_bufinuse = 0;
    mrec->mrec_nchild = 0;
    mrec->mrec_length = mbmp->mbmp_osd->osd_bsize;
    LIST_INIT(&mrec->mrec_blocks);
     
    return mrec;
}

/* 
 * XXX Have a call to use the buffer and set the allocbuf flag, which
 * rn is on by default. This is nice for blocks but may write one 
 * garbage block per record.
 */

void
mrec_free(struct osd_mrec *mrec)
{
    mbmp_freebit(mrec->mrec_mbmp, mrec->mrec_ondisk);
    /*
    if (mrec->mrec_allocbuf)
	free(mrec->mrec_buf, M_SLSMM);
    free(mrec, M_SLSMM);
    */
}

/*
 * Open vp as an OSD device, and return an in-memory copy of it superblock.
 */
struct slsosd *
slsosd_import(struct vnode *vp)
{
    struct slsosd *osd;
    int error;
    void *buf;
    //int doubleerror;

    osd = malloc(sizeof(*osd), M_SLSMM, M_WAITOK);

    /*
    VOP_LOCK(vp, LK_EXCLUSIVE);
    error = VOP_OPEN(vp, FREAD | FWRITE, 
	    curthread->td_proc->p_ucred, curthread, NULL);
    if (error != 0) {
	printf("Opening OSD vnode failed with %d\n", error);
	return NULL;
    }
    */

    /* 
     * XXX Change this to use a block access method; normally we 
     * should be able to pass a bitmap as the first argument, which
     * in this place is currently impossible.
     */
    buf = malloc(PAGE_SIZE, M_SLSMM, M_WAITOK);
    error = osd_pread(NULL, 0, buf, PAGE_SIZE);
    memcpy(osd, buf, sizeof(*osd));
    free(buf, M_SLSMM);

    if (error != 0) {
	printf("Error: Could not read OSD\n");
	goto slsosd_import_error;
    }

    if (osd->osd_magic != SLSOSD_MAGIC) {
	printf("Error: Opened device is not an OSD. Expected: %llx, got %lx\n",
		SLSOSD_MAGIC, osd->osd_magic);
	goto slsosd_import_error;
    }

    if (osd->osd_majver > SLSOSD_MAJOR_VERSION) {
	printf("Error: OSD major version %hu is too high\n", osd->osd_majver);
	goto slsosd_import_error;
    }

    if (osd->osd_minver > SLSOSD_MINOR_VERSION) {
	printf("Error: OSD minor version %hu is too high\n", osd->osd_minver);
	goto slsosd_import_error;
    }
    
    /* HACK... but maybe it makes more sense to give up the lock */
    //VOP_UNLOCK(vp, LK_EXCLUSIVE);

    return osd;

slsosd_import_error:
    free(osd, M_SLSMM);

    /*
    doubleerror = VOP_CLOSE(vp, FREAD | FWRITE | O_NONBLOCK, 
		    curthread->td_proc->p_ucred, curthread);
    VOP_UNLOCK(vp, LK_EXCLUSIVE);
    if (doubleerror != 0)
	panic("Could not close opened OSD vnode\n");
    */

    return NULL;
}

/*
 * Read the bitmap of osd into memory.
 */
struct osd_mbmp *
mbmp_import(struct slsosd *osd)
{
    uint64_t bmp_size;
    struct osd_mbmp *mbmp;
    uint64_t *bmp;
    //int error;

    mbmp = malloc(sizeof(*mbmp), M_SLSMM, M_WAITOK);

    /* The size in bits is the size of the OSD in blocks */
    bmp_size = osd->osd_size / 64;
    bmp = malloc(bmp_size * sizeof(*bmp), M_SLSMM, M_WAITOK);

    mbmp->mbmp_osd = osd;
    mbmp->mbmp_bmp = bmp;
    mbmp->mbmp_size = bmp_size;
    /* 
     * Round up so that we don't accidentally get a word that includes
     * the superblock or the bitmap.
     */
    mbmp->mbmp_bstart = 64 * ((osd->osd_firstblk.ptr_offset + 63) / 64);
    mbmp->mbmp_istart = 64 * ((osd->osd_inodeoff.ptr_offset + 63) / 64);
    mbmp->mbmp_icur = mbmp->mbmp_istart; 
    mbmp->mbmp_bcur = mbmp->mbmp_bstart;

    /*
     *	XXX See the comment for osd_pread in slsosd_import.
     */
    /*
    error = osd_pread(NULL, osd->osd_allocoff.ptr_offset, bmp, bmp_size);
    if (error != 0) {
	free(mbmp, M_SLSMM);
	free(bmp, M_SLSMM);
	return NULL;
    }
    */

    return mbmp;
}

/*
 * Would be nice to reference count to 
 * make sure nobody is still accessing it.
 */
void mbmp_free(struct osd_mbmp *mbmp)
{
    if (mbmp == NULL)
	return;
    
    free(mbmp->mbmp_bmp, M_SLSMM);
    free(mbmp, M_SLSMM);
}

/*
 * Get a contiguous range of blocks
 */
uint64_t
blk_getrange(struct osd_mbmp *mbmp, uint64_t *size)
{
    uint64_t block;

    /* Size gets updated in here, too. */
    block = mbmp_getbit(mbmp, mbmp->mbmp_bstart, mbmp->mbmp_size, 
		&mbmp->mbmp_bcur, size);

    return block;
}

void
mino_addmrec(struct osd_mino *mino, struct osd_mrec *mrec)
{
    LIST_INSERT_HEAD(&mino->mino_records, mrec, mrec_records);
}

void
mrec_addbuf(struct osd_mrec *mrec, void *buf, size_t len)
{
    mrec->mrec_buf = buf;
    mrec->mrec_len = len;
}

static void
mrec_list_free(struct mrec_list *mrecs)
{
    struct osd_mrec *mrec, *mrec_temp;

    if (mrecs == NULL)
	return;

    LIST_FOREACH_SAFE(mrec, mrecs, mrec_records, mrec_temp) {
	LIST_REMOVE(mrec, mrec_records);
	free(mrec->mrec_buf, M_SLSMM);
	free(mrec, M_SLSMM);
    }
}

static int
rec_read(struct osd_mbmp *mbmp, struct slsosd_record *rec, uint64_t block)
{

    return EINVAL;
}


static int
rec_write(struct osd_mbmp *mbmp, struct slsosd_record *rec, uint64_t block)
{
    int error;
    
    error = osd_pwrite(mbmp, block, rec, sizeof(*rec));
    if (error != 0)
	printf("osd_pwrite failed with %d\n", error);

    return error;
}


int 
mblk_read(struct osd_mrec *mblk)
{

    return EINVAL;
}


int 
mblk_write(struct osd_mrec *mblk)
{
    int error;

    error = osd_pwrite(mblk->mrec_mbmp, mblk->mrec_ondisk, mblk->mrec_buf, mblk->mrec_len);
    if (error != 0)
	printf("osd_pwrite failed with %d\n", error);

    return error;
}


static int
ino_read(struct osd_mbmp *mbmp, struct slsosd_inode *ino)
{


    return EINVAL;
}

static int
ino_write(struct osd_mbmp *mbmp, struct slsosd_inode *ino, uint64_t block)
{
    int error;
    
    error = osd_pwrite(mbmp, block, ino, sizeof(*ino));
    if (error != 0)
	printf("osd_pwrite failed with %d\n", error);

    return error;
}

/*
 * Helper that writes to disk the blocks that 
 * go directly in the given record struct. 
 */
static struct osd_mrec *
mrec_write_direct(struct slsosd_record *rec, struct mrec_list *mblks, uint64_t depth)
{
    struct osd_mrec *mblk;
    size_t dblk;
    int error;

    KASSERT(depth <= 3, "blocks are up to triple indirect");

    dblk = 0;
    LIST_FOREACH(mblk, mblks, mrec_records) {
	/* Stop if we run out of room in the record struct. */
	if (dblk == 64)
	    break;

	switch (depth) {
	case 0:
	    rec->rec_ptr[dblk++].ptr_offset = mblk->mrec_ondisk;
	    break;

	case 1:
	    rec->rec_iptr[dblk++].ptr_offset = mblk->mrec_ondisk;
	    break;

	case 2:
	    rec->rec_diptr[dblk++].ptr_offset = mblk->mrec_ondisk;
	    break;

	case 3:
	    rec->rec_tiptr[dblk++].ptr_offset = mblk->mrec_ondisk;
	    break;

	}

	error = mblk_write(mblk);
	if (error != 0)
	    printf("mblk_write failed with %d\n", error);

    }


    return mblk;
}


/*
 * Helper that writes all the blocks in the list, and groups their 
 * positions in indirect blocks. Used for creating indirect blocks
 * of any level of indirection.
 */
static struct mrec_list *
mrec_write_indirect(struct mrec_list *mblks, struct osd_mrec *mblk)
{
    struct mrec_list *iblks;
    struct osd_mrec *imblk;
    uint64_t *imblkdata;
    struct slsosd *osd;
    size_t index;

    osd = mblk->mrec_mbmp->mbmp_osd;

    iblks = malloc(sizeof(*iblks), M_SLSMM, M_WAITOK);
    LIST_INIT(iblks);

    index = 0;
    LIST_FOREACH_FROM(mblk, mblks, mrec_records) {
	if (index == 0) {
	    imblk = malloc(sizeof(*imblk), M_SLSMM, M_WAITOK);
	    imblkdata = malloc(osd->osd_bsize, M_SLSMM, M_WAITOK);

	    imblk->mrec_buf = (void *) imblkdata;
	    LIST_INSERT_HEAD(iblks, imblk, mrec_records);
	}

	imblkdata[index] = mblk->mrec_ondisk;
	mblk_write(mblk);
	
	index = (index + 1) % (osd->osd_bsize / sizeof(uint64_t));
    }

    return iblks;
}


int
mrec_write(struct osd_mrec *mrec)
{
    struct vnode *vp;
    struct slsosd *osd;
    struct slsosd_record *rec;
    struct mrec_list *iblks, *diblks, *tiblks;
    struct osd_mrec *mblk;
    struct osd_mbmp *mbmp;
    int error;

    mbmp = mrec->mrec_mbmp;
    osd = mbmp->mbmp_osd;
    vp = mbmp->mbmp_osdvp;
    iblks = diblks = tiblks = NULL;

    rec = malloc(sizeof(*rec), M_SLSMM, M_WAITOK);
    rec->rec_type = mrec->mrec_type;
    rec->rec_size = mrec->mrec_size;
    /* XXX Initialize the rest of the fields */

    /* If buffer in use, have it as a child block */
    if (mrec->mrec_bufinuse != 0) {
	mblk = mrec_alloc(mbmp, 0, 0);
	mblk->mrec_buf = mrec->mrec_buf;
	mblk->mrec_len = mrec->mrec_len;
	LIST_INSERT_HEAD(&mrec->mrec_blocks, mblk, mrec_records);
    }

    mblk = mrec_write_direct(rec, &mrec->mrec_blocks, 0);
    if (mblk == NULL)
	goto mrec_write_out;

    iblks = mrec_write_indirect(&mrec->mrec_blocks, mblk);
    mblk = mrec_write_direct(rec, iblks, 1);
    if (mblk == NULL)
	goto mrec_write_out;

    diblks = mrec_write_indirect(iblks, mblk);

    mrec_write_direct(rec, diblks, 2);
    if (mblk == NULL)
	goto mrec_write_out;

    tiblks = mrec_write_indirect(diblks, mblk);
    mrec_write_direct(rec, tiblks, 3);


mrec_write_out:
    if (mblk != NULL) {
	printf("Error: Record was too large for the OSD, got truncated\n");
    }

    error = rec_write(mbmp, rec, mrec->mrec_ondisk);
    if (error != 0)
	printf("rec_write failed with %d\n", error);


    /*
    mrec_list_free(&mrec->mrec_blocks);
    mrec_list_free(iblks);
    mrec_list_free(diblks);
    mrec_list_free(tiblks);

    free(rec, M_SLSMM);
    */

    return 0;
}


/*
 * Helper that writes to disk the records that 
 * go directly in the given inode struct. 
 */
static struct osd_mrec *
mino_write_direct(struct slsosd_inode *ino, struct mrec_list *mrecs, uint64_t depth)
{
    struct osd_mrec *mrec;
    size_t drec;
    int error;

    KASSERT(depth <= 3, "records are up to triple indirect");

    drec = 0;
    LIST_FOREACH(mrec, mrecs, mrec_records) {
	/* Stop if we run out of room in the record struct. */
	if (drec == 64)
	    break;

	switch (depth) {
	case 0:
	    ino->ino_records[drec++].ptr_offset = mrec->mrec_ondisk;
	    break;

	case 1:
	    ino->ino_irecords[drec++].ptr_offset = mrec->mrec_ondisk;
	    break;

	case 2:
	    ino->ino_direcords[drec++].ptr_offset = mrec->mrec_ondisk;
	    break;

	case 3:
	    ino->ino_tirecords[drec++].ptr_offset = mrec->mrec_ondisk;
	    break;

	}

	error = mrec_write(mrec);
	if (error != 0)
	    printf("mrec_write failed with %d\n", error);
    }


    return mrec;
}


/*
 * Helper that writes all the records in the list, and groups their 
 * positions in indirect records. Used for creating indirect blocks
 * of any level of indirection.
 */
static struct mrec_list *
mino_write_indirect(struct mrec_list *mrecs, struct osd_mrec *mrec)
{
    struct mrec_list *irecs;
    struct osd_mrec *imrec;
    struct slsosd *osd;
    int index;

    osd = mrec->mrec_mbmp->mbmp_osd;

    irecs = malloc(sizeof(*irecs), M_SLSMM, M_WAITOK);
    LIST_INIT(irecs);

    index = 0;
    LIST_FOREACH_FROM(mrec, mrecs, mrec_records) {
	if (index == 0) {
	    imrec = malloc(sizeof(*imrec), M_SLSMM, M_WAITOK);
	    LIST_INSERT_HEAD(irecs, imrec, mrec_records);
	}

	mrec_write(mrec);
	
	index = (index + 1) % (osd->osd_bsize / sizeof(uint64_t));
    }

    return irecs;
}

int
mino_read(struct osd_mino *mino)
{


    return EINVAL;
}

int
mino_write(struct osd_mino *mino)
{
    struct vnode *vp;
    struct slsosd_inode *ino;
    struct slsosd *osd;
    struct mrec_list *irecs, *direcs, *tirecs;
    struct osd_mrec *mrec;
    int error;

    vp = mino->mino_mbmp->mbmp_osdvp;
    osd = mino->mino_mbmp->mbmp_osd;
    irecs = direcs = tirecs = NULL;

    ino = malloc(sizeof(*ino), M_SLSMM, M_WAITOK);
    memcpy(ino->ino_procname, mino->mino_procname, 64);
    /* XXX Initialize the rest of the fields */

    mrec = mino_write_direct(ino, &mino->mino_records, 0);
    if (mrec == NULL)
	goto mino_write_out;

    irecs = mino_write_indirect(&mino->mino_records, mrec);
    mrec = mino_write_direct(ino, irecs, 1);
    if (mrec == NULL)
	goto mino_write_out;

    direcs = mino_write_indirect(irecs, mrec);
    mino_write_direct(ino, direcs, 2);
    if (mrec == NULL)
	goto mino_write_out;

    tirecs = mino_write_indirect(direcs, mrec);
    mino_write_direct(ino, tirecs, 3);


mino_write_out:
    if (mrec != NULL) {
	printf("Error: Inode was too large for the OSD, got truncated\n");
    }

    error = ino_write(mino->mino_mbmp, ino, mino->mino_ondisk);
    if (error != 0)
	printf("ino_write failed with %d\n", error);

    /*
    mrec_list_free(&mino->mino_records);
    mrec_list_free(irecs);
    mrec_list_free(direcs);
    mrec_list_free(tirecs);

    free(ino, M_SLSMM);
    */

    return 0;
}
