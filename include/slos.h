
#ifndef _SLOS_H_
#define _SLOS_H_

#include <sys/uuid.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/lock.h>
#include <sys/lockmgr.h>
#include <sys/mutex.h>
#include <vm/uma.h>

typedef uint64_t bnode_ptr;

#ifdef WITH_DEBUG
#define DBUG(fmt, ...) do {			    \
    printf("(%s: Line %d) ", __FILE__, __LINE__);   \
    printf(fmt, ##__VA_ARGS__);			    \
    } while (0) 
#else
#define DBUG(fmt, ...) ((void)(0));
#endif // WITH_DEBUG

typedef uint64_t vnode_off_key_t[2];

extern uma_zone_t fnodes_zone;


/*
 * SLOS Pointer
 */
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

#define SLOS_LOCK(slos) (lockmgr(&((slos)->slos_lock), LK_EXCLUSIVE, NULL))
#define SLOS_UNLOCK(slos) (lockmgr(&((slos)->slos_lock), LK_RELEASE, NULL))

/*
 * Object store flags
 */
#define SLOS_FLAG_TRIM	0x00000001 /* TRIM Support */
#define SLOS_FLAG_HASH	0x00000002 /* Checksum Support */

struct slos {
	SLIST_ENTRY(slos)	next_slos;

	struct vnode		*slos_vp;	/* The vnode for the disk device */
	struct vnode		*slsfs_dev;
	struct slos_sb		*slos_sb;	/* The superblock of the filesystem */
	struct slos_bootalloc	*slos_bootalloc;/* The bootstrap alloc for the device */
	struct slos_blkalloc	*slos_alloc;    /* The allocator for the device */
	struct g_consumer	*slos_cp;	/* The geom consumer used to talk to disk */
	struct g_provider	*slos_pp;	/* The geom producer */ 
	struct btree		*slos_inodes;	/* An index of all inodes */
	struct slos_vhtable	*slos_vhtable;	/* Table of opened vnodes */
	struct lock		slos_lock;	/* Sleepable lock */
};

/* Get the intra-block position of an offset for the given SLOS. */
#define blkoff(slos, off)   (off % slos->slos_sb->sb_bsize)

/* Get the block number of the offset for the given SLOS. */
#define blkno(slos, off)    (off / slos->slos_sb->sb_bsize)

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
	bnode_ptr		sb_bmap_ptr;	/* The disk location of the root of the bmap vnode */
};

extern struct slos slos;



#endif /* _SLOS_H_ */

