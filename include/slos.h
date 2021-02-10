
#ifndef _SLOS_H_
#define _SLOS_H_

#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/lockmgr.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/uuid.h>

#include <vm/vm.h>
#include <vm/uma.h>
#include <vm/vm_object.h>

/* Maximum number of superblocks, one per epoch. */
#define NUMSBS (100)

/*
 * On-disk btree pointer. It needs to be here to be accessible by userspace
 * tools, even though it's btree related.
 */
typedef uint64_t bnode_ptr;

/* Physical extent on-disk pointer */
struct slos_diskptr {
	uint64_t
	    offset;    /* The block offset of the first block of the region. */
	uint64_t size; /* The size of the region in bytes. */
	uint64_t epoch;
};
typedef struct slos_diskptr diskptr_t;

/* Shorthands for creating disk pointers. */
#define DISKPTR(blkno, size)     \
	((struct slos_diskptr) { \
	    blkno,               \
	    size,                \
	})
#define DISKPTR_BLOCK(blkno) DISKPTR(blkno, 1)
#define DISKPTR_NULL DISKPTR(0, 0)

#define SLOS_MAGIC 0x19AA8455115505AAULL

#define SLOS_MAXVOLLEN 32

#define SLOS_MAJOR_VERSION 1
#define SLOS_MINOR_VERSION 4

/*
 * Object store flags
 */
#define SLOS_FLAG_TRIM 0x00000001 /* TRIM Support */
#define SLOS_FLAG_HASH 0x00000002 /* Checksum Support */

/*
 * SLOS Superblock
 */
struct slos_sb {
	uint64_t sb_magic;  /* magic */
	uint16_t sb_majver; /* major version */
	uint16_t sb_minver; /* minor version */
	uint32_t sb_flags;  /* feature flags */
	uint64_t sb_epoch;  /* epoch number */
	uint32_t sb_index; /* Index superblock is in the array of size NUMSBS */
	uint64_t sb_time;  /* Time */
	uint64_t sb_time_nsec; /* Time nsec */
	uint64_t sb_meta_synced;
	uint64_t sb_data_synced;
	uint64_t sb_attempted_checkpoints;

	/* Identification information */
	struct uuid sb_uuid; /* object store uuid */
	u_char sb_name[SLOS_MAXVOLLEN];

	/* Configuration */
	uint64_t sb_ssize; /* physical disk sector size */
	uint64_t sb_bsize; /* block size used for I/O */
	uint64_t sb_asize; /* allocation size */
	uint64_t sb_size;  /* fs size in blocks */

	/* Summary Information */
	uint64_t sb_numblks; /* number of blocks */
	uint64_t sb_mtime;   /* last mounted */

	diskptr_t sb_root;	  /* Root inode for all inodes */
	diskptr_t sb_allocoffset; /* Allocator Offset key tree */
	diskptr_t sb_allocsize;	  /* Allocator Size key tree */
	diskptr_t sb_cksumtree;	  /* Checksum inode */
};

_Static_assert(sizeof(struct slos_sb) < DEV_BSIZE, "Block size wrong");

#define SLOS_BSIZE(slos) ((&slos)->slos_sb->sb_bsize)
#define SLOS_DEVBSIZE(slos) ((&slos)->slos_sb->sb_ssize)

/* The offset size in blocks in the VM object inodes where the data starts */
#define SLOS_OBJOFF (16)

/* Turns an SLS ID to an identifier suitable for the SLOS. */
#define OIDTOSLSID(OID) ((int)(OID & INT_MAX))

#ifdef _KERNEL

SDT_PROVIDER_DECLARE(slos);

#define SLOS_LOCK(slos) (lockmgr(&((slos)->slos_lock), LK_EXCLUSIVE, NULL))
#define SLOS_UNLOCK(slos) (lockmgr(&((slos)->slos_lock), LK_RELEASE, NULL))

MALLOC_DECLARE(M_SLOS_SB);

/* An extent of data in the SLOS. */
struct slos_extent {
	uint64_t sxt_lblkno; /* The logical block number of the first block. */
	size_t sxt_cnt;	     /* The total size of the extent in blocks. */
};

void slos_ptr_trimstart(
    uint64_t newbln, uint64_t bln, size_t fsbsize, diskptr_t *ptr);

struct slos_blkalloc {
	struct slos_node *a_offset;
	struct slos_node *a_size;
	struct slos_diskptr chunk;
};

struct slos {
	SLIST_ENTRY(slos) next_slos;

	struct vnode *slos_vp; /* The vnode for the disk device */
	struct vnode *slsfs_inodes;

	uint64_t slsfs_dirtybufcnt;

	int slsfs_sync_exit;
	int slsfs_syncing;
	int slsfs_checkpointtime;
	struct thread *slsfs_syncertd;
	struct cv slsfs_sync_cv;
	struct mtx slsfs_sync_lk;
	struct mount *slsfs_mount;

	struct slos_sb *slos_sb; /* The superblock of the filesystem */
	struct slos_blkalloc slos_alloc;
	struct slos_node *slos_cktree;

	struct g_consumer *slos_cp; /* The geom consumer used to talk to disk */
	struct g_provider *slos_pp; /* The geom producer */

	struct lock slos_lock;	   /* Sleepable lock */
	struct taskqueue *slos_tq; /* Slos taskqueue */
	int slos_usecnt;	   /* Number of SLS users of the SLOS */
};
extern struct slos slos;

extern uint64_t checkpoints;
extern uint64_t checkpointtime;

#endif

#endif /* _SLOS_H_ */
