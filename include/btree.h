#ifndef _BTREE_H_
#define _BTREE_H_

typedef int (*compare_t)(const void *, const void *);
typedef void (*rootchange_t)(void *, bnode_ptr p);

typedef uint32_t fb_keysize;
typedef uint32_t fb_valsize;

#define FBTREE_DIRTYCNT(tree) ((tree)->bt_backend->v_bufobj.bo_dirty.bv_cnt)

extern uma_zone_t fnodes_zone;

#define NODE_TYPE(node) ((node)->fn_dnode->dn_flags)
#define NODE_DATA_SIZE(vp) ((4096) - sizeof(struct dnode))
#define NODE_KS(node) ((node)->fn_tree->bt_keysize)
#define NODE_VS(node) ((NODE_TYPE((node)) == BT_INTERNAL) ? sizeof(bnode_ptr) : ((node)->fn_tree->bt_valsize))

// Total data size minus offset of vales - offset of keys (size of key space);
#define NODE_VAL_SIZE(node) (NODE_DATA_SIZE((node)->fn_tree->fn_backend) - ((node)->fn_values - (node)->fn_keys))

#define MAX_NUM_INTERNAL(node) \
    ((NODE_DATA_SIZE((node)->fn_tree->bt_backend) - sizeof(bnode_ptr)) / ((node)->fn_tree->bt_keysize + sizeof(bnode_ptr)))

#define MAX_NUM_EXTERNAL(node) \
    ((NODE_DATA_SIZE((node)->fn_tree->bt_backend)) / ((node)->fn_tree->bt_keysize + (node)->fn_tree->bt_valsize + 1))

#define MAX_NUM_BUCKET(node) \
    ((NODE_DATA_SIZE((node)->fn_tree->bt_backend) - NODE_KS(node)) / ((node)->fn_tree->bt_valsize))

#define NODE_OFFSET(node) (MAX_NUM_INTERNAL(node) * (node)->fn_tree->bt_keysize)
#define NODE_SIZE(node)	((node)->fn_dnode->dn_numkeys)
#define NODE_RELEASE(node) (brelse((node)->fn_buf))
#define NODE_ISBUCKETAT(node, index) ((node)->fn_types[(index)])

#define NODE_COMPARE(node, key1, key2) ((node)->fn_tree->comp(key1, key2))

#define ITER_VAL(iter) (fnode_getval((iter).it_node, (iter).it_index))
#define ITER_KEY(iter) (fnode_getkey((iter).it_node, (iter).it_index))
#define ITER_ISBUCKETAT(iter) ((iter).it_node->fn_types[(iter).it_index] != 0)

#define ITER_VAL_T(iter, TYPE) (*(TYPE *)ITER_VAL(iter))
#define ITER_KEY_T(iter, TYPE) (*(TYPE *)ITER_KEY(iter))

#define ITER_RELEASE(iter)  (BTREE_UNLOCK((iter).it_node->fn_tree, 0));
#define ITER_NEXT(iter) (fnode_iter_next(&(iter), 1))
#define ITER_ISNULL(iter) ((iter).it_index == -1)

#define BTREE_LOCK(tree, flags) (lockmgr(&(tree)->bt_lock, flags, 0))
#define BTREE_UNLOCK(tree, flags) (lockmgr(&(tree)->bt_lock, LK_RELEASE | flags, 0))
#define BTREE_LKSTATUS(tree) (lockstatus(&(tree)->bt_lock))
#define BUCKET_KEY(node, type) (*(type *)fnode_getkey(node, 0))

#define BUCKET_SETNEXT(node, val) (fnode_setval(node, NODE_MAX(node), val))
#define BUCKET_HASNEXT(node) ((*(bnode_ptr *)fnode_getval(node, NODE_MAX(node))) != 0)
#define BUCKET_GETNEXT(node, next) (fnode_fetch(node, NODE_MAX(node), next))

#define FNODE_PTRSIZE (300)

/*
 * FBtree Root Change Entry - Register callbacks for
 * when the root changes
 */
struct fbtree_rcentry {
	SLIST_ENTRY(fbtree_rcentry)  rc_entry;
	rootchange_t		    rc_fn;
	void			    *rc_ctx;
};

/* 
 * File Backed Btree
 *
 * These style of btrees will use files as their main base for their data, 
 * using buffer cache entries and the bufflushes as a way to gain persistance.
 *
 * Allocation state is held in the first block of the file, this is just a 
 * simple ordered free list.
 */
struct fbtree {
	struct vnode	*bt_backend;	/* The vnode representing our backend */
	fb_keysize	bt_keysize;	/* Size of keys */
	fb_valsize	bt_valsize;	/* Size of values */
	bnode_ptr	bt_root;
	uint64_t	bt_flags;
	struct lock	bt_lock;
	size_t		bt_inserts;
	size_t		bt_splits;
	size_t		bt_removes;
	size_t		bt_replaces;
	struct rwlock	bt_trie_lock;
	struct pctrie	bt_trie;
	size_t		bt_gets;
	size_t		bt_root_replaces;
	char		bt_name[255];
	void		*bt_hash;
	u_long		bt_hashmask;

	SLIST_HEAD(bt_head, fbtree_rcentry) bt_rcfn;

	compare_t	comp;
};


/* Dnode status flags */
#define BT_EXTERNAL ((uint8_t)0)
#define BT_INTERNAL ((uint8_t)1)
#define BT_BUCKET ((uint8_t)2)

/*
 * On Disk Btree Node
 */
struct dnode {
	uint32_t	dn_numkeys;	/* Number of keys */ 
	uint8_t		dn_flags;	/* External/Internal/Bucket*/
	char		dn_data[];	/* Data which will hold the keys and children */
};


/* fnode fn_status flags
 */
#define FN_ALLOWDUPLICATE	(0x1)
#define FN_DEAD			(0x2)

/*
 * In Memory File Btree Node
 */
struct fnode {
	struct fbtree		*fn_tree;	/* Associated tree */
	struct dnode		*fn_dnode;	/* On disk data */
	struct buf		*fn_buf;	/* Data buffer itself for btree data */
	bnode_ptr		fn_location;	/* Disk block to fnode */
	int8_t			fn_status;	/* Status flags (see flags above)*/
	uint8_t			*fn_types;
	void			*fn_values;	/* Pointer to values list */
	void			*fn_keys;	/* Pointer to keys list */
};

/*
 * Iterator
 */
struct fnode_iter {
	struct fnode	*it_node;
	int32_t		it_index; 
};

/* UMA Zone Constructor and deconstructor */
int fnode_construct(void * mem, int size, void *arg, int flags);
void fnode_deconstruct(void *mem, int size, void *arg);

/* Iterator Functions */
int fnode_iter_next(struct fnode_iter *it, int bucket_traverse);
int fnode_iter_prev(struct fnode_iter *it);
int fnode_iter_get(struct fnode_iter *it, void *val);
void *fnode_getkey(struct fnode *node, int i);
void *fnode_getval(struct fnode *node, int i);
int fiter_remove(struct fnode_iter *it);
void fiter_replace(struct fnode_iter *it, void *val);

/* Helpers for the btree algorithm */
__always_inline static inline int
NODE_MAX(struct fnode *node)
{
	if (NODE_TYPE(node) == BT_INTERNAL) {
		return MAX_NUM_INTERNAL(node);
	} else if (NODE_TYPE(node) == BT_BUCKET) {
		// Last value is the next pointer
		return MAX_NUM_BUCKET(node) - 1;
	} else {
		return MAX_NUM_EXTERNAL(node);
	}
}

void fnode_setup(struct fnode *node, struct fbtree *tree, bnode_ptr ptr);
int fnode_keymin(struct fnode *node, void *key, void *value);
int fnode_insert(struct fnode *node, void *key, void *value);
int fnode_init(struct fbtree *tree, bnode_ptr ptr, struct fnode **node);
int fnode_right(struct fnode *node, struct fnode **right);
int fnode_fetch(struct fnode *node, int index, struct fnode **next);
int fnode_parent(struct fnode *node, struct fnode **parent);
void fnode_write(struct fnode *node);

/* File Backed Btree operations */
int fbtree_init(struct vnode *backend, bnode_ptr ptr, fb_keysize keysize, 
    fb_valsize valsize, compare_t comp, char *, uint64_t fbflags, struct fbtree *tree);
void fbtree_destroy(struct fbtree *tree);
size_t fbtree_size(struct fbtree *tree);
void fbtree_reg_rootchange(struct fbtree *tree, rootchange_t fn, void *ctx);

/* Read operations */
int fbtree_keymax_iter(struct fbtree *tree, void *key, struct fnode_iter *iter);
int fbtree_keymin_iter(struct fbtree *tree, void *key, struct fnode_iter *iter);
int fbtree_iterat(struct fbtree *tree, const void *key, struct fnode_iter *iter);
int fbtree_get(struct fbtree *tree, const void *key, void *value);
int fnode_keymax(struct fnode *root, void *key, void *value);

/* Write operations */
int fbtree_insert(struct fbtree *tree, void *key, void *value);
int fbtree_remove(struct fbtree *tree, void *key, void *value);
int fbtree_replace(struct fbtree *tree, void *key, void *value);
int fbtree_sync(struct fbtree *tree);
int fbtree_sync_withalloc(struct fbtree *tree, diskptr_t *pre);
int fbtree_rangeinsert(struct fbtree *tree, uint64_t lbn, uint64_t size);
int fnode_create_bucket(struct fnode *node, int at, void *key, struct fnode **fin);

/* Debug functions */
int fbtree_test(struct fbtree *tree);
void fnode_print(struct fnode *node);
void fnode_print_internal(struct fnode *node);
void fnode_print_level(struct fnode *node);
int slsfs_fbtree_test(void);

/* Internal system btree initialization */
int fbtree_sysinit(struct slos *slos, size_t offset, diskptr_t *ptr);

#endif /* _BTREE_H_ */
