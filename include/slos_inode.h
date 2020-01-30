#ifndef _SLOS_INODE_H_
#define _SLOS_INODE_H_

#include <sys/param.h>

#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h> 
#include <sys/vnode.h>

#include "slos.h"

struct btree;

#define SLOS_VALIVE	0
#define SLOS_VDEAD	1

#define SVPBLK(svp) (svp->sn_slos->slos_sb->sb_bsize);
#define INUM(node) (node->sn_ino->ino_pid);
#define RECORDDATASIZ(vp) ((vp->sn_slos->slos_sb->sb_bsize) - (sizeof(struct slos_record)))

struct slos_node {
	int64_t			sn_pid;			/* process id */
	int64_t			sn_uid;			/* user id */
	int64_t			sn_gid;			/* group id */
	u_char			sn_procname[64];	/* process name */

	uint64_t		sn_ctime;		/* creation time */
	uint64_t		sn_mtime;		/* last modification time */

	uint64_t		sn_blk;			/* on-disk position */

	uint64_t		sn_status;		/* status of vnode */
	uint64_t		sn_refcnt;		/* reference count */
	LIST_ENTRY(slos_node)	sn_entries;		/* link for in-memory vnodes */
	struct btree		*sn_records;		/* records btree */
	struct mtx		sn_mtx;			/* vnode mutex */
	struct slos_inode	*sn_ino;		/* On disk representation of the slos */
	struct slos		*sn_slos;		/* Slos the node belong to */
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
	uint64_t		ino_ctime_nsec;
	uint64_t		ino_mtime;		/* last modification time */
	uint64_t		ino_mtime_nsec;

	uint64_t		ino_blk;		/* on-disk position */
	struct slos_diskptr	ino_records;		/* btree of records */

	uint64_t		ino_flags;		/* inode flags */
	uint64_t		ino_magic;		/* magic for finding errors */

	uint16_t		ino_mode;		/* mode */
	uint64_t		ino_link_num;
	uint64_t		ino_asize;		/* Actual size of file on disk */
	uint64_t		ino_size;		/* Size of file */
};


LIST_HEAD(slos_vnlist, slos_node);

/* Number of buckets in the hashtable. */
#define VHTABLE_MAX (1024)

int slos_vhtable_init(struct slos *slos);
int slos_vhtable_fini(struct slos *slos); 

struct slos_node *slos_vhtable_find(struct slos *slos, uint64_t pid);

void slos_vhtable_add(struct slos *slos, struct slos_node *vp);
void slos_vhtable_remove(struct slos *slos, struct slos_node *vp);

/* A hashtable of resident vnodes. */
struct slos_vhtable {
	struct slos_vnlist	*vh_table;	/* The hashtable of open vnodes. */
	u_long			vh_hashmask;	/* The hashmask for the table. */
};

int slos_icreate(struct slos *slos, uint64_t pid, uint16_t mode);
int slos_iremove(struct slos *slos, uint64_t pid);

int slos_iupdate(struct slos_node *svp);
void slos_timechange(struct slos_node *svp);

struct slos_node *slos_iopen(struct slos *slos, uint64_t pid);
int slos_iclose(struct slos *slos, struct slos_node *vno);

struct slos_node *slos_istat(struct slos *slos, uint64_t inoblk);

struct slos_node *slos_vpimport(struct slos *slos, uint64_t inoblk);
int slos_vpexport(struct slos *slos, struct slos_node *vp);
void slos_vpfree(struct slos *slos, struct slos_node *vp);
int slos_test_inode(void);

#endif /* _SLOS_INODE_H_ */
