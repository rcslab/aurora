#ifndef _SLS_MOSD_H_
#define _SLS_MOSD_H_

#include  "sls_osd.h"

/*
 * A representation of the bitmap in memory. Each bit corresponds to one
 * on-disk block. Operations are word-sized.
 */
struct osd_mbmp {
    struct slsosd   *mbmp_osd;	    /* associated osd */
    struct vnode    *mbmp_osdvp;    /* vnode of associated osd */
    uint64_t	    *mbmp_bmp;	    /* pointer to the actual bitmap */
    uint64_t	    mbmp_size;	    /* size in words */
    uint64_t	    mbmp_bstart;    /* start of the block bitmap in words */
    uint64_t	    mbmp_istart;    /* start of the inode bitmap in words */
    uint64_t	    mbmp_icur;	    /* position of free inode iterator in words */
    uint64_t	    mbmp_bcur;	    /* position of free block iterator in words */
};

struct osd_mrec;
LIST_HEAD(mrec_list, osd_mrec);

/*
 * In-memory records (distinct data structure from on-disk records).
 * There is no need for the three-tier structure in memory, so we 
 * instead use a flat list for the children.
 */
struct osd_mrec {
    struct osd_mbmp		*mrec_mbmp;	/* associated bitmap */
    uint64_t			mrec_bufinuse;	/* whether we should write the buf */
    uint64_t			mrec_allocbuf;	/* whether we own the buf */
    uint32_t			mrec_type;	/* record type */
    uint64_t			mrec_length;    /* record length in bytes */
    uint64_t			mrec_size;	/* on-disk size in blocks */
    uint64_t			mrec_ondisk;    /* on-disk position */
    uint64_t			mrec_nchild;	/* number of child records */
    uint64_t			mrec_len;	/* length of block in bytes */
    void			*mrec_buf;	/* data buffer for the block */
    struct mrec_list		mrec_blocks;	/* list of in-memory blocks */
    LIST_ENTRY(osd_mrec)	mrec_records;	/* list of in-memory siblings */
};

/*
 * In-memory inodes.
 */
struct osd_mino {
    struct osd_mbmp	*mino_mbmp;	    /* associated bitmap */
    int64_t		mino_pid;	    /* process id */
    int64_t		mino_uid;	    /* user id */
    int64_t		mino_gid;	    /* group id */
    u_char		mino_procname[64];  /* process name */
    uint64_t		mino_ctime;	    /* creation time */
    uint64_t		mino_mtime;	    /* last modification time */
    uint64_t		mino_ondisk;	    /* on-disk position */
    struct mrec_list	mino_records;	    /* records */
};

uint64_t blk_getrange(struct osd_mbmp *mbmp, uint64_t *size);
struct osd_mino *mino_alloc(struct osd_mbmp *mbmp, int flush);
void mino_free(struct osd_mino *mino);
int mino_read(struct osd_mino *mino);
int mino_write(struct osd_mino *mino);

struct osd_mrec *mrec_alloc(struct osd_mbmp *mbmp, int flush, int allocbuf);
void mrec_free(struct osd_mrec *mrec);
int mrec_read(struct osd_mrec *mrec);
int mrec_write(struct osd_mrec *mrec);

struct osd_mrec *mblk_alloc(struct osd_mbmp *mbmp, uint64_t size, int flush);
void mblk_free(struct osd_mrec *mblk);
int mblk_read(struct osd_mrec *mblk);
int mblk_write(struct osd_mrec *mblk);

struct slsosd *slsosd_import(struct vnode *vp);
int slsosd_export(struct slsosd *osd);

void mbmp_free(struct osd_mbmp *mbmp);

struct osd_mbmp *mbmp_import(struct slsosd *osd);
int mbmp_export(struct osd_mbmp *mbmp);

void mino_addmrec(struct osd_mino *mino, struct osd_mrec *mrec);
void mrec_addbuf(struct osd_mrec *mrec, void *buf, size_t len);


#endif /* _SLS_MOSD_H_ */

