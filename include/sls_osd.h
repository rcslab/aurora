
#ifndef _SLS_OSD_H__
#define _SLS_OSD_H__

#include <sys/uuid.h>

/*
 * SLSOSD Pointer
 */
struct slsosd_ptr {
	/*uint64_t	ptr__reserved;*/
	uint64_t	ptr_offset;
	/*uint8_t	ptr_hash[16];*/
};

#define SLSOSD_MAGIC		0x19AA8455115505AAULL

#define SLSOSD_MAXVOLLEN	32

#define SLSOSD_MAJOR_VERSION	1
#define SLSOSD_MINOR_VERSION	0

/*
 * Object store flags
 */
#define SLSOSD_FLAG_TRIM	0x00000001 /* TRIM Support */
#define SLSOSD_FLAG_HASH	0x00000002 /* Checksum Support */

/*
 * SLSOSD Superblock
 */
struct slsosd {
	uint64_t		osd_magic;	/* magic */
	uint16_t		osd_majver;	/* major version */
	uint16_t		osd_minver;	/* minor version */
	uint32_t		osd_flags;	/* feature flags */
	struct slsosd_ptr	osd_allocoff;	/* block allocation bitmap */
	struct slsosd_ptr	osd_inodeoff;	/* inode table offset */
	struct slsosd_ptr	osd_firstblk;	/* first free block */
	uint64_t		osd_numblks;	/* number of blocks */
	uint64_t		osd_numinodes;	/* number of inodes */
    /* Identification information */
	struct uuid		osd_uuid;	/* object store uuid */
	u_char			osd_name[SLSOSD_MAXVOLLEN];
    /* Configuration */
	uint64_t		osd_bsize;	/* block size used for I/O */
	uint64_t		osd_asize;	/* allocation size */
	uint64_t		osd_size;	/* fs size in blocks */
    /* Summary Infomration */
	uint8_t			osd_clean;	/* osd clean */
	uint64_t		osd_mtime;	/* last mounted */
};

/*
 * SLSOSD Inode
 *
 * Each inode represents a single object in our store.  Each object contains 
 * one or more records that contain the actual file data.
 */
struct slsosd_inode {
	int64_t			ino_pid;		/* process id */
	int64_t			ino_uid;		/* user id */
	int64_t			ino_gid;		/* group id */
	u_char			ino_procname[64];	/* process name */
	uint64_t		ino_ctime;		/* creation time */
	uint64_t		ino_mtime;		/* last modification time */
	uint64_t		ino_lastrec;		/* last record */
	struct slsosd_ptr	ino_records[64];	/* direct records */
	struct slsosd_ptr	ino_irecords[64];	/* indirect records */
	struct slsosd_ptr	ino_direcords[64];	/* double indirect records */
	struct slsosd_ptr	ino_tirecords[64];	/* triple indirect records */
};

#define SLSREC_INVALID	0x00000000
#define SLSREC_PROC	0x00000001
#define SLSREC_TD	0x00000002
#define SLSREC_MEM	0x00000003
#define SLSREC_MEMENTRY	0x00000004
#define SLSREC_MEMOBJT	0x00000005
#define SLSREC_FDESC	0x00000006
#define SLSREC_FILE	0x00000007
#define SLSREC_FILENAME 0x00000008
#define SLSREC_DATA	0x00000009
#define SLSREC_ADDR	0x0000000A
#define SLSREC_PAGE	0x0000000B

/*
 * SLSOSD Record
 *
 * Every object consists of one or more records.
 */
struct slsosd_record {
	uint32_t		rec_type;	/* record type */
	uint64_t		rec_length;	/* record length in bytes */
	uint64_t		rec_size;	/* on-disk size in blocks */
	struct slsosd_ptr	rec_ptr[64];	/* direct blocks */
	struct slsosd_ptr	rec_iptr[64];	/* indirect blocks */
	struct slsosd_ptr	rec_diptr[64];	/* double indirect blocks */
	struct slsosd_ptr	rec_tiptr[64];	/* triple indirect blocks */
};

#endif

