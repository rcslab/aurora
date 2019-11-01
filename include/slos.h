
#ifndef _SLOS_H_
#define _SLOS_H_

#include <sys/uuid.h>

/*
 * SLOS Pointer
 */

/* The on-disk version of a block pointer. */
struct slos_diskptr {
	/*uint64_t  _reserved;*/
	uint64_t    offset;	/* The block offset of the first block of the region. */
	uint64_t    size;	/* The size of the region in blocks. */
	/*uint8_t   hash[16];*/
};

/* Block size for file-backed SLOSes. */
#define SLOS_FILEBLKSIZE (64 * 1024)

/* Shorthands for creating disk pointers. */
#define DISKPTR(blkno, size) \
    ((struct slos_diskptr) { blkno, size, })
#define DISKPTR_BLOCK(blkno) DISKPTR(blkno, 1)
#define DISKPTR_NULL DISKPTR(0, 0)

#define SLOS_MAGIC		0x19AA8455115505AAULL

#define SLOS_MAXVOLLEN	32

#define SLOS_MAJOR_VERSION	1
#define SLOS_MINOR_VERSION	4

/*
 * Object store flags
 */
#define SLOS_FLAG_TRIM	0x00000001 /* TRIM Support */
#define SLOS_FLAG_HASH	0x00000002 /* Checksum Support */

/*
 * SLOS Superblock
 */
struct slos_sb {
	uint64_t		sb_magic;	/* magic */
	uint16_t		sb_majver;	/* major version */
	uint16_t		sb_minver;	/* minor version */
	uint32_t		sb_flags;	/* feature flags */
	struct slos_diskptr	sb_broot;	/* root of the allocator offset btree */
	struct slos_diskptr	sb_szroot;	/* root of the allocator size btree */
	struct slos_diskptr	sb_inodes;	/* pointer to the inode btree */
	struct slos_diskptr	sb_data;	/* data region */
	struct slos_diskptr	sb_bootalloc;	/* bootstrap allocator region */
    /* Identification information */
	struct uuid		sb_uuid;	/* object store uuid */
	u_char			sb_name[SLOS_MAXVOLLEN];
    /* Configuration */
	uint64_t		sb_ssize;	/* physical disk sector size */
	uint64_t		sb_bsize;	/* block size used for I/O */
	uint64_t		sb_asize;	/* allocation size */
	uint64_t		sb_size;	/* fs size in blocks */
    /* Summary Information */
	uint64_t		sb_numboot;	/* number of boot allocator blocks */
	uint64_t		sb_numblks;	/* number of blocks */
	uint8_t			sb_clean;	/* osd clean */
	uint64_t		sb_mtime;	/* last mounted */
};


/* Inode flags */
#define SLOSINO_DIRTY	0x00000001

/* Magic for each inode. */
#define SLOS_IMAGIC	0x51051A1CUL

/* Maximum length of the inode name. */
#define SLOS_NAMELEN	64

/*
 * SLSOSD Inode
 *
 * Each inode represents a single object in our store.  Each object contains 
 * one or more records that contain the actual file data.
 */
struct slos_inode {
	int64_t			ino_pid;		/* process id */
	int64_t			ino_uid;		/* user id */
	int64_t			ino_gid;		/* group id */
	u_char			ino_procname[64];	/* process name */

	uint64_t		ino_ctime;		/* creation time */
	uint64_t		ino_mtime;		/* last modification time */

	uint64_t		ino_blk;		/* on-disk position */
	uint64_t		ino_lastrec;		/* last record */
	struct slos_diskptr	ino_records;		/* btree of records */

	uint64_t		ino_flags;		/* inode flags */
	uint64_t		ino_magic;		/* magic for finding errors */
};

#define SLOSREC_INVALID	    0x00000000	/* Record is invalid */
#define SLOSREC_PROC	    0x00000001	/* Record holds process-local info */
#define SLOSREC_MEM	    0x00000003	/* Record holds info related to a vmspace */
#define SLOSREC_VMOBJ	    0x00000005	/* Record holds info for an object */
#define SLOSREC_FILE	    0x00000006	/* Record holds info for a file */
#define SLOSREC_PIPE	    0x00000007	/* Record holds info for a pipe */
#define SLOSREC_KQUEUE	    0x00000008	/* Record holds info for a kqueue */
#define SLOSREC_SOCKET	    0x00000009	/* Record holds info for a socket */
#define SLOSREC_VNODE	    0x0000000a	/* Record holds info for a vnode */

/* XXX Factor out */
#define SLOSREC_FDESC	    0x000000a0	/* Record holds a file descriptor*/
#define SLOSREC_DATA	    0x000000a1
#define SLOSREC_FILENAME    0x000000a2


#define SLOS_RMAGIC	    0x51058A1CUL

/*
 * SLSOSD Record
 *
 * Every object consists of one or more records.
 */
struct slos_record {
	uint32_t		rec_type;	/* record type */
	uint64_t		rec_length;	/* record data length in bytes */
	uint64_t		rec_size;	/* record data size in blocks */
	uint64_t		rec_magic;	/* record magic */
	uint64_t		rec_num;	/* record number in the inode */
	uint64_t		rec_blkno;	/* on-disk position of record */
	struct slos_diskptr	rec_data;	/* root of record data btree */
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

#endif /* _SLOS_H_ */

