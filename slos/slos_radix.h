#ifndef _SLOS_RADIX_H_
#define _SLOS_RADIX_H_

#include <sys/param.h>
#include <sys/buf.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include <slos.h>

struct slos_rdxtree {
	struct vnode *stree_vp;
	daddr_t stree_root;
	struct rwlock stree_lock;
	uint64_t stree_max;
	uint64_t stree_mask;
	uint64_t stree_srdxcap;
};

struct slos_rdxnode {
	/* No need for a lock, uses that of the backing buffer. */
	struct slos_rdxtree *srdx_tree; /* Tree this node belongs to */
	uint64_t srdx_parent;		/* Parent node */
	uint64_t srdx_key; /* Key which triggered the retrieval of this node. */
	struct buf *srdx_buf; /* Buffer cache buffer for this node */
	diskblk_t *srdx_vals; /* Pointer into the value array in the buffer */
};

#define SRDX_LOCK(srdx) (BUF_LOCK((srdx)->srdx_buf, LK_EXCLUSIVE, 0))
#define SRDX_ASSERT_LOCKED(srdx) (BUF_ASSERT_LOCKED((srdx)->srdx_buf))
#define SRDX_ASSERT_UNLOCKED(srdx) (BUF_ASSERT_UNLOCKED((srdx)->srdx_buf))
#define SRDX_UNLOCK(srdx) (BUF_UNLOCK((srdx)->srdx_buf))

int stree_init(struct vnode *vp, daddr_t daddr, struct slos_rdxtree **streep);
void stree_rootcreate(struct slos_rdxtree *stree, diskptr_t ptr);
void stree_destroy(struct slos_rdxtree *stree);

#define STREE_INVAL \
	((diskblk_t) { 0xbabababababababaULL, 0xcdcdcdcdcdcdcdcdULL })
#define STREE_VALSIZE (sizeof(diskblk_t))
#define STREE_VALVALID(value) ((value).offset != STREE_INVAL.offset)

#define BP_SRDX_SET(bp, srdx)                  \
	do {                                   \
		((bp)->b_fsprivate2 = (srdx)); \
	} while (0)
#define BP_SRDX_GET(bp) ((struct slos_rdxnode *)(bp)->b_fsprivate2)

int slos_radix_init(void);
void slos_radix_fini(void);

#endif /* _SLOS_RADIX_H_ */
