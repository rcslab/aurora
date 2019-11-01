#ifndef _SLOS_INODE_H_
#define _SLOS_INODE_H_

#include <sys/param.h>

#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>

#include "../include/slos.h"
#include "slos_btree.h"
#include "slos_internal.h"

#define SLOS_VALIVE	0
#define SLOS_VDEAD	1

struct slos_vnode {
	int64_t			vno_pid;		/* process id */
	int64_t			vno_uid;		/* user id */
	int64_t			vno_gid;		/* group id */
	u_char			vno_procname[64];	/* process name */

	uint64_t		vno_ctime;		/* creation time */
	uint64_t		vno_mtime;		/* last modification time */

	uint64_t		vno_blk;		/* on-disk position */
	uint64_t		vno_lastrec;		/* last record */

	uint64_t		vno_status;		/* status of vnode */
	uint64_t		vno_refcnt;		/* reference count */
	LIST_ENTRY(slos_vnode)	vno_entries;		/* link for in-memory vnodes */
	struct btree		*vno_records;		/* records btree */
	struct mtx		vno_mtx;		/* vnode mutex */
};

LIST_HEAD(slos_vnlist, slos_vnode);

/* Number of buckets in the hashtable. */
#define VHTABLE_MAX (1024)

int slos_vhtable_init(struct slos *slos);
int slos_vhtable_fini(struct slos *slos); 

struct slos_vnode *slos_vhtable_find(struct slos *slos, uint64_t pid);

void slos_vhtable_add(struct slos *slos, struct slos_vnode *vp);
void slos_vhtable_remove(struct slos *slos, struct slos_vnode *vp);

/* A hashtable of resident vnodes. */
struct slos_vhtable {
	struct slos_vnlist	*vh_table;	/* The hashtable of open vnodes. */
	u_long			vh_hashmask;	/* The hashmask for the table. */
};

int slos_icreate(struct slos *slos, uint64_t pid);
int slos_iremove(struct slos *slos, uint64_t pid);

struct slos_vnode *slos_iopen(struct slos *slos, uint64_t pid);
int slos_iclose(struct slos *slos, struct slos_vnode *vno);

struct slos_vnode *slos_istat(struct slos *slos, uint64_t inoblk);

struct slos_vnode *slos_vpimport(struct slos *slos, uint64_t inoblk);
int slos_vpexport(struct slos *slos, struct slos_vnode *vp);

int slos_test_inode(void);

#endif /* _SLOS_INODE_H_ */
