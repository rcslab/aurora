#ifndef _FB_BTREE_H_
#define _FB_BTREE_H_

#include <sys/vnode.h>
#include <sys/mutex.h>
#include <slos.h>
#include <slsfs.h>

typedef int (*compare_t)(const void *, const void *);

typedef uint32_t fb_keysize;
typedef uint32_t fb_valsize;

#define BT_INTERNAL 0x1

extern uma_zone_t fnodes_zone;

#define FALLOC() (uma_zalloc(fnodes_zone, M_WAITOK | M_ZERO))
#define FFREE(node) (uma_zfree(fnodes_zone, node));
#define NODE_FLAGS(node) ((node)->fn_dnode->dn_flags)
#define NODE_DATA_SIZE(vp) ((4096) - sizeof(struct dnode) + 1)
#define NODE_KS(node) ((node)->fn_tree->bt_keysize)
#define NODE_VS(node) ((NODE_FLAGS((node)) & BT_INTERNAL) ? sizeof(bnode_ptr) : ((node)->fn_tree->bt_valsize))

// Total data size minus offset of vales - offset of keys (size of key space);
#define NODE_VAL_SIZE(node) (NODE_DATA_SIZE((node)->fn_tree->fn_backend) - ((node)->fn_values - (node)->fn_keys))

#define MAX_NUM_INTERNAL(node) \
    ((NODE_DATA_SIZE((node)->fn_tree->bt_backend) - sizeof(bnode_ptr)) / \
	((node)->fn_tree->bt_keysize + sizeof(bnode_ptr)))

#define MAX_NUM_EXTERNAL(node) \
    ((NODE_DATA_SIZE((node)->fn_tree->bt_backend)) / ((node)->fn_tree->bt_keysize + (node)->fn_tree->bt_valsize))

#define NODE_OFFSET(node) (MAX_NUM_INTERNAL(node) * (node)->fn_tree->bt_keysize)
#define NODE_SIZE(node)	((node)->fn_dnode->dn_numkeys)
#define NODE_MAX(node) (NODE_FLAGS((node)) & BT_INTERNAL ? (node)->fn_max_int : (node)->fn_max_ext)

#define NODE_COMPARE(node, key1, key2) ((node)->fn_tree->comp(key1, key2))

#define ITER_VAL(iter) (fnode_getval(&iter.it_node, iter.it_index))
#define ITER_KEY(iter) (fnode_getkey(&iter.it_node, iter.it_index))

struct alloc_d {
	bnode_ptr	    a_root_ptr;
	size_t	    a_numEntries;
	bnode_ptr 	    data[];
};


/* Simple block Allocator */
struct fileblk_alloc {
	struct vnode    *a_vnode;
	struct buf	    *a_buf;
	bnode_ptr	    a_ptr;
	struct alloc_d  *a_data;
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
	struct rwlock	bt_lock;
	size_t		bt_inserts;
	size_t		bt_splits;
	size_t		bt_removes;
	size_t		bt_replaces;
	size_t		bt_gets;
	size_t		bt_root_replaces;

	compare_t	comp;
};

/*
 * On Disk Btree Node
 */
struct dnode {
	bnode_ptr	dn_rightnode;	/* Offset of its right node */
	bnode_ptr	dn_parent;	/* Offset of its parent */
	uint32_t	dn_numkeys;	/* Number of keys */ 
	uint32_t	dn_flags;	/* External vs Internal vs Bucket*/
	char		dn_data[];	/* Data which will hold the keys and children */
};

/*
 * In Memory File Btree Node
 */
struct fnode {
	struct fbtree	*fn_tree;	/* Associated tree */
	struct dnode	*fn_dnode;	/* On disk data */
	struct buf	*fn_buf;	/* Buf of associated dnode */
	bnode_ptr	fn_location;
	size_t		fn_max_int;
	size_t		fn_max_ext;
	void		*fn_values;	/* Pointer to values list */
	void		*fn_keys;	/* Pointer to keys list */
};

/*
 * Iterator
 */
struct fnode_iter {
	struct fnode	it_node;
	uint32_t		it_index; 
};

int fnode_iter_next(struct fnode_iter *it);
int fnode_iter_prev(struct fnode_iter *it);
int fnode_iter_get(struct fnode_iter *it, void *val);


/* Helpers for the btree algorithm */
int fnode_keymin(struct fnode *node, void *key, void *value);
int fnode_keymin_iter(struct fnode *node, void *key, struct fnode_iter *iter);
int fnode_init(struct fbtree *tree, bnode_ptr ptr, struct fnode *node);

/* File Backed Btree operations */
int fbtree_init(struct vnode *backend, bnode_ptr ptr, fb_keysize keysize, 
    fb_valsize valsize, compare_t comp, struct fbtree *tree);
void fbtree_destroy(struct fbtree *tree);


/* Read operations */
int fbtree_keymin(struct fbtree *tree, const void *key, void *value);
int fbtree_keymax(struct fbtree *tree, const void *key, void *value);
int fbtree_iterat(struct fbtree *tree, const void *key, struct fnode_iter *iter);
int fbtree_get(struct fbtree *tree, const void *key, void *value);

/* Write operations */
int fbtree_insert(struct fbtree *tree, void *key, void *value);
int fbtree_delete(struct fbtree, void *key, void *value);
int fbtree_replace(struct fbtree *tree, void *key, void *value);

int fbtree_test(struct fbtree *tree);

#endif /* _FB_BTREE_H_ */
