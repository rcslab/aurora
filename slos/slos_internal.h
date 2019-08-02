#ifndef _SLOS_INTERNAL_H_
#define _SLOS_INTERNAL_H_

#include <sys/param.h>

#include <sys/queue.h>

#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/uuid.h>

#include "../include/slos.h"
#include "../include/slos_bnode.h"

/* Uncomment to make tests available. */
#define SLOS_TESTS

struct slos {
	struct vnode		*slos_vp;	/* The vnode for the disk device */
	struct slos_sb		*slos_sb;	/* The superblock of the filesystem */
	struct slos_bootalloc	*slos_bootalloc;/* The bootstrap alloc for the device */
	struct slos_blkalloc	*slos_alloc;    /* The allocator for the device */
	struct g_consumer	*slos_cp;	/* The geom consumer used to talk to disk */
	struct mtx		slos_mtx;	/* Mutex for SLOS-wide operations */
	struct btree		*slos_inodes;	/* An index of all inodes */
	struct slos_vhtable	*slos_vhtable;	/* Table of opened vnodes */
};

extern struct slos slos;

/* Get the intra-block position of an offset for the given SLOS. */
#define blkoff(slos, off)   (off % slos.slos_sb->sb_bsize)

/* Get the block number of the offset for the given SLOS. */
#define blkno(slos, off)    (off / slos.slos_sb->sb_bsize)



#endif /* _SLOS_INTERNAL_H_ */
