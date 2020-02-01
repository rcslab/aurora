#ifndef _SLOS_RECORD_H_
#define _SLOS_RECORD_H_

#include <sys/param.h>
#include <slos.h>

struct slos_node;

/* Inode flags */
#define SLOSREC_INVALID	    0x00000000	/* Record is invalid */
#define SLOSREC_PROC	    0x00000001	/* Record holds process-local info */
#define SLOSREC_SESS	    0x00000002	/* Record holds process-local info */
#define SLOSREC_MEM	    0x00000003	/* Record holds info related to a vmspace */
#define SLOSREC_VMOBJ	    0x00000005	/* Record holds info for an object */
#define SLOSREC_FILE	    0x00000006  /* Record holds info for a file */
#define SLOSREC_SYSVSHM	    0x0000000b	/* Record holds info for SYSV shared memory */
#define SLOSREC_SOCKBUF	    0x0000000d	/* Record holds info for socket buffers */
#define SLOSREC_DIR	    0x0000000e	/* Record holds a directory */
#define SLOSREC_DATA	    0x0000000f	/* Record holds arbitrary data */

#define SLOS_RMAGIC	    0x51058A1CUL

/*
 * SLSOSD Record
 *
 * Every object consists of one or more records.
 */
struct slos_record {
	uint32_t		rec_type;		/* record type */
	uint64_t		rec_length;		/* record data length in bytes */
	uint64_t		rec_size;		/* record data size in blocks */
	uint64_t		rec_magic;		/* record magic */
	uint64_t		rec_num;		/* record number in the inode */
	uint64_t		rec_blkno;		/* on-disk position of record */
	struct slos_diskptr	rec_data;		/* root of record data btree */
	char			rec_internal_data[];	/* internal data of record */ 
};

/* 
 * An entry into the record data btree. It is needed because the data written
 * in the extent may be unaligned both at the start and at the end, and so we
 * need the offsets of the beginning and end of actual data in the btree. By
 * using the start and end fields, we can also calculate the real size in bytes.
 */
struct slos_recentry {
	struct slos_diskptr diskptr;	/* The disk pointer of the backing extent. */
	uint64_t offset;		/* The in-extent offset where data starts. */
	uint64_t len;			/* The length of actual data in bytes. */
};

int slos_rcreate(struct slos_node *vp, uint64_t rtype, uint64_t *rnop);

/* XXX Implement */
uint64_t slos_rclone(struct slos_node *vp, uint64_t rno);
int slos_rremove(struct slos_node *vp, struct slos_record *record);
int slos_rread(struct slos_node *vp, uint64_t rno, struct uio *auio);
int slos_rwrite(struct slos_node *vp, uint64_t rno, struct uio *auio);
/* XXX Implement */
int slos_rtrim(struct slos_node *vp, uint64_t rno, struct uio *auio);

/* Flags for slos_seek. */
#define SREC_SEEKLEFT	0b001
#define SREC_SEEKRIGHT	0b010
#define SREC_SEEKHOLE	0b100

/* Special value for seeklenp when we don't find an extent/hole. */
#define SREC_SEEKEOF	0
#define SLOS_RECORD_FOREACH(slsvp, record, tmp, error) \
    for(tmp = NULL, error = slos_firstrec(slsvp, &record); record != NULL && error != EIO;  \
	    tmp = record, error = slos_nextrec(slsvp, record->rec_num, &record), free(tmp, M_SLOS)) \

#define SLOS_RECORDNUM_FOREACH(slsvp, num) \
    for(int err = slos_firstrno(slsvp, &num); err != EINVAL; err = slos_nextrno(slsvp, &num))


int slos_rseek(struct slos_node *vp, uint64_t rno, uint64_t offset, 
	int flags, uint64_t *seekoffp, uint64_t *seeklenp);

/* Stat structure for individual records. */
struct slos_rstat {
	uint64_t type;	/* The type of the record */
	uint64_t len;	/* The length of the record */
};

int slos_rstat(struct slos_node *vp, uint64_t rno, struct slos_rstat *stat);

/* Records btree iterators */
int slos_firstrno(struct slos_node *vp, uint64_t *rnop);
int slos_lastrno(struct slos_node *vp, uint64_t *rnop);
int slos_prevrno(struct slos_node *vp, uint64_t *rnop);
int slos_nextrno(struct slos_node *vp, uint64_t *rnop);
int slos_firstrno_typed(struct slos_node *vp, uint64_t rtype, uint64_t *rnop);
int slos_lastrno_typed(struct slos_node *vp, uint64_t rtype, uint64_t *rnop);

int slos_getrec(struct slos_node *vp, uint64_t rno, struct slos_record **rec);
int slos_freerec(struct slos_record *rec);

int slos_prevrec(struct slos_node *vp, uint64_t rno, struct slos_record **record);
int slos_nextrec(struct slos_node *vp, uint64_t rno, struct slos_record **record);
int slos_firstrec(struct slos_node *vp, struct slos_record **record);

#ifdef SLOS_TESTS

int slos_test_record(void);

#endif /* SLOS_TESTS */

#endif /* _SLOS_RECORD_H_ */
