#ifndef _SLOS_INODE_H_
#define _SLOS_INODE_H_

#include <slos.h>

#include "btree.h"

/* Record types */
#define SLOSREC_INVALID 0x00000000  /* Record is invalid */
#define SLOSREC_PROC 0x00000001	    /* Process-local info */
#define SLOSREC_SESS 0x00000002	    /* Process-local info */
#define SLOSREC_VMSPACE 0x00000003  /* VM Space */
#define SLOSREC_VMOBJ 0x00000004    /* VM Object */
#define SLOSREC_FILE 0x00000005	    /* File */
#define SLOSREC_SYSVSHM 0x00000006  /* System V shared memory */
#define SLOSREC_SOCKBUF 0x00000007  /* Socket buffers */
#define SLOSREC_VNODE 0x00000008    /* VNode */
#define SLOSREC_DATA 0x00000009	    /* Data */
#define SLOSREC_MANIFEST 0x0000000a /* Checkpoint Manifest */
#define SLOSREC_META 0x0000000b	    /* Generic metadata block */

#define SLOS_RMAGIC 0x51058A1CUL

/* Stat structure for individual records. */
struct slos_rstat {
	uint64_t type; /* The type of the record */
	uint64_t len;  /* The length of the record */
};

#define SLOS_DIRTY (0x1000)

#define IN_ACCESS (0x2)
#define IN_UPDATE (0x4)
#define IN_CHANGE (0x8)
#define IN_DEAD (0x10)
#define IN_MODIFIED (0x20)
#define IN_CREATE (0x40)
#define IN_RENAME (0x80)

#define IEXEC 0000100
#define IWRITE 0000200
#define IREAD 0000400
#define ISVTX 0001000
#define ISGID 0002000
#define ISUID 0004000

#define SVPBLK(svp) (svp->sn_slos->slos_sb->sb_bsize)
#define INUM(node) (node->sn_ino.ino_pid)
#define RECORDDATASIZ(vp) \
	((vp->sn_slos->slos_sb->sb_bsize) - (sizeof(struct slos_record)))

#define SLS_ISSAS(vp) \
	(SLSVP(vp) != NULL && SLSVP(vp)->sn_addr != (vm_offset_t)NULL)

/* On-disk SLOS inode. */
struct slos_inode {
	int64_t ino_pid; /* process id */
	int64_t ino_uid; /* user id */
	int64_t ino_gid; /* group id */
	dev_t ino_special;
	u_char ino_procname[64]; /* process name */

	uint64_t ino_ctime; /* inode change time */
	uint64_t ino_ctime_nsec;
	uint64_t ino_mtime; /* last modification time */
	uint64_t ino_mtime_nsec;
	uint64_t ino_atime; /* last accessed time */
	uint64_t ino_atime_nsec;
	uint64_t ino_birthtime; /* Creation time */
	uint64_t ino_birthtime_nsec;

	uint64_t ino_blk;	       /* on-disk position */
	struct slos_diskptr ino_btree; /* btree for data */

	uint64_t ino_flags; /* inode flags */
	uint64_t ino_magic; /* magic for finding errors */

	mode_t ino_mode;     /* mode */
	uint64_t ino_nlink;  /* hard links */
	uint64_t ino_asize;  /* Actual size of file on disk */
	uint64_t ino_size;   /* Size of file */
	uint64_t ino_blocks; /* Number of on IO blocks */
	struct slos_rstat
	    ino_rstat; /* Type and length of the records held in the node */
	struct slos_diskptr ino_wal_segment; /* If the inode is a WAL, location
						of the WAL segment */
};

/* In-memory SLOS inode. */
struct slos_node {
	struct slos_inode sn_ino; /* On disk representation of the slos */
#define sn_pid sn_ino.ino_pid
#define sn_uid sn_ino.ino_uid
#define sn_gid sn_ino.ino_gid
#define sn_procname sn_ino.ino_procname
#define sn_blk sn_ino.ino_blk
	uint64_t sn_status;		  /* status of vnode */
	LIST_ENTRY(slos_node) sn_entries; /* link for in-memory vnodes */
	struct fbtree sn_tree;		  /* Data btree */
	struct mtx sn_mtx;		  /* vnode mutex */
	struct slos *sn_slos;		  /* Slos the node belong to */

	struct vnode *sn_fdev; /* Fake vnode for btree back */
	vm_object_t sn_obj;    /* SAS object */
	vm_offset_t sn_addr;   /* SAS object mapping */
};

/* Inode flags */
#define SLOSINO_DIRTY 0x00000001

/* Magic for each inode. */
#define SLOS_IMAGIC 0x51051A1CUL

/* Maximum length of the inode name. */
#define SLOS_NAMELEN 64

#ifdef _KERNEL
LIST_HEAD(slos_vnlist, slos_node);

int slos_init(void);
int slos_uninit(void);

int slos_icreate(struct slos *slos, uint64_t pid, uint16_t mode);
int slos_iremove(struct slos *slos, uint64_t pid);

int slos_updatetime(struct slos_inode *ino);
int slos_update(struct slos_node *svp);

int slos_iopen(struct slos *slos, uint64_t slsid, struct slos_node **svpp);

struct slos_node *slos_istat(struct slos *slos, uint64_t inoblk);

int slos_svpimport(
    struct slos *slos, uint64_t svpid, bool system, struct slos_node **svpp);
int slos_vpexport(struct slos *slos, struct slos_node *vp);
void slos_vpfree(struct slos *slos, struct slos_node *vp);
int slos_test_inode(void);
void slsfs_root_rc(void *ctx, bnode_ptr p);

int slos_svpalloc(struct slos *slos, mode_t mode, uint64_t *slsidp);

int initialize_inode(struct slos *slos, uint64_t pid, diskptr_t *p);
#endif

#endif /* _SLOS_INODE_H_ */
