#include <sys/param.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <btree.h>
#include <slos.h>
#include <sys/errno.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <machine/atomic.h>

#include "slosmm.h"
#include "slos.h"
#include "slos_inode.h"
#include "slos_alloc.h"

#define NODE_LOCK(node, flags) (BUF_LOCK((node)->fn_buf, flags, NULL))
#define NODE_ISLOCKED(node) (BUF_ISLOCKED((node)->fn_buf))

static int fnode_split(struct fnode *node);

static void
ftree_stats(struct fbtree *tree)
{
    DBUG("Type : Count\n");
    DBUG("Inserts : %lu\n", tree->bt_inserts);
    DBUG("Removes : %lu\n", tree->bt_removes);
    DBUG("Replaces : %lu\n", tree->bt_replaces);
    DBUG("Gets : %lu\n", tree->bt_gets);
    DBUG("Splits : %lu\n", tree->bt_splits);
    DBUG("Root replaces : %lu\n", tree->bt_root_replaces);
}

static void
fnode_write(struct fnode *node)
{
	node->fn_buf->b_flags |= B_MANAGED;
	bdwrite(node->fn_buf);
}

static void *
fnode_getkey(struct fnode *node, int i)
{
	KASSERT(i < NODE_MAX(node), ("Not getting key past max"));
	KASSERT(node->fn_keys != NULL, ("Use fnode INIT"));
	return  (char *)node->fn_keys + (i * node->fn_tree->bt_keysize);
}

static void *
fnode_getval(struct fnode *node, int i)
{
	KASSERT(i <= NODE_MAX(node), ("Not getting val past max"));
	KASSERT(node->fn_values != NULL, ("Use fnode INIT"));
	return (char *)node->fn_values + (i * NODE_VS(node));
}

static void
fnode_setkey(struct fnode *node, int i, int8_t type, void *val)
{
	KASSERT(i < NODE_MAX(node), ("Not inserting key past max"));
	void *at = fnode_getkey(node, i);
	memcpy((char *)at, val, node->fn_tree->bt_keysize);
}

static void
fnode_setval(struct fnode *node, int i, void *val)
{
	KASSERT(i < NODE_MAX(node), ("Not inserting val past max"));
	memcpy(fnode_getval(node, i), val, NODE_VS(node));
}

static void
fnode_print(struct fnode *node)
{
	int i;
	DBUG("NODE: %lu\n", node->fn_location);
	if (NODE_FLAGS(node) & BT_INTERNAL) {
		bnode_ptr *p = fnode_getval(node, 0);
		DBUG("| C %lu |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = fnode_getval(node, i + 1);
			size_t __unused *t = fnode_getkey(node, i);
			DBUG("| K %lu || C %lu |", *t, *p);
		}
	} else {
		for (i = 0; i < NODE_SIZE(node); i++) {
			size_t __unused *t = fnode_getkey(node, i);
			size_t __unused *v = fnode_getval(node, i);
			DBUG("| %lu -> %lu |,", *t, *v);
		}
	}
	DBUG("\n");
}

/*
 * The buffer is up to the caller to handle.  The caller must either use
 * brelse -> Put back onto the free list to be reused (if you are holding
 * any pointer to the data it may not be valid after this call) Buffer will
 * be locked shared as it exits
 *
 * brelse -> Put on the clean list
 *
 * bdwrite -> dirty the buffer to be synced on next checkpoint
 */
static int
getptr(struct vnode *back, bnode_ptr ptr, struct buf **bp)
{
	struct buf *tmp;
	int error;

	struct slos *slos = (struct slos *)back->v_data;

	VOP_LOCK(back, LK_SHARED);
	error = bread(back, ptr, slos->slos_sb->sb_bsize, curthread->td_ucred, &tmp);
	if (error) {
		DBUG("Error reading block\n");
		return (EIO);
	}
	VOP_UNLOCK(back, 0);
	*bp = tmp;

	return (0);
}

static int
fnode_follow(struct fnode *node, const void *key) 
{
	int error = 0;
	void *keyt;

	while (NODE_FLAGS(node) & BT_INTERNAL) {
		int index = 0;
		// Linear search for now
		for (index = 0; index < NODE_SIZE(node); index++) {
			keyt = fnode_getkey(node, index);
			if (NODE_COMPARE(node, keyt, key) > 0) {
				break;
			}
		}

		bnode_ptr *val = fnode_getval(node, index);
		brelse(node->fn_buf);
		error = fnode_init(node->fn_tree, *val, node);
		if (error) {
			break;
		}
	}

	KASSERT((NODE_FLAGS(node) & BT_INTERNAL) == 0, ("Node still internal\n"));

	return (error);
}

int
fbtree_init(struct vnode *backend, bnode_ptr ptr, fb_keysize keysize, 
    fb_valsize valsize, compare_t comp, struct fbtree *tree)
{
	tree->bt_backend = backend;
	tree->bt_keysize = keysize;
	tree->bt_valsize = valsize;
	tree->comp = comp;
	tree->bt_root = ptr;
	rw_init(&tree->bt_lock, "Btree RW-LOCK");

	return (0);
}

int
fnode_keymin_iter(struct fnode *root, void *key, struct fnode_iter *iter) 
{
	int error;
	struct fnode node = *root;
	// Follow catches if this is an external node or not
	error = fnode_follow(&node, key);
	if (error) {
		return (error);
	}

	if (NODE_SIZE(&node) == 0) {
	    bqrelse(node.fn_buf);
	    return (EINVAL);
	}

	KASSERT((NODE_FLAGS(&node) & (BT_INTERNAL)) == 0, ("Should be on a external node now"));
	void *val;
	int diff;
	int mid = 0;
	for(mid = 0; mid < NODE_SIZE(&node); mid++) {
	    val = fnode_getkey(&node, mid);
	    diff = NODE_COMPARE(&node, val, key);
	    if (diff >= 0) {
		break;
	    }
	}

	/*int last = NODE_SIZE(&node) - 1;*/
	/*int index = 0;*/
	/*while (index <= last) {*/
		/*mid = (last + index) / 2;*/
		/*val = fnode_getkey(&node, mid);*/
		/*diff = NODE_COMPARE(&node, val, key);*/
		/*if (diff > 0) {*/
			/*last = mid - 1;*/
		/*} else if (diff == 0) {*/
			/*break;*/
		/*} else {*/
			/*index = mid + 1;*/
		/*}*/
	/*}*/
	iter->it_node = node;
	iter->it_index = mid;

	return (0);
}

int
fnode_keymin(struct fnode *root, void *key, void *value)
{
	int error;
	struct fnode_iter iter;

	error = fnode_keymin_iter(root, key, &iter);
	if (error) {
		return (error);
	}

	memcpy(value, ITER_VAL(iter), root->fn_tree->bt_valsize);
	memcpy(key, ITER_KEY(iter), root->fn_tree->bt_keysize);

	bqrelse(iter.it_node.fn_buf);

	return (0);
}

int
fbtree_get(struct fbtree *tree, const void *key, void *value)
{
	char check[tree->bt_keysize];
	struct fnode node;
	int error;

	rw_rlock(&tree->bt_lock);
	error = fnode_init(tree, tree->bt_root, &node);
	if (error) {
		rw_runlock(&tree->bt_lock);
		return (error);
	}

	memcpy(check, key, tree->bt_keysize);
	error = fnode_keymin(&node, check, value);
	if (error) {
		rw_runlock(&tree->bt_lock);
		return (error);
	}

	if (NODE_COMPARE(&node, key, check)) {
		value = NULL;
		rw_runlock(&tree->bt_lock);
		return (EINVAL);
	} 

	rw_runlock(&tree->bt_lock);

	atomic_fetchadd_long(&tree->bt_gets, 1);

	return (0);
}

static void
fnode_insert_at(struct fnode *node, void *key, void *value, int i) 
{
	char * src;
	src = fnode_getkey(node, i);
	memmove(src + node->fn_tree->bt_keysize, src, (NODE_SIZE(node) - i) * NODE_KS(node));
	fnode_setkey(node, i, NODE_FLAGS(node), key);

	if (NODE_FLAGS(node) & BT_INTERNAL) {
		src = fnode_getval(node, i + 1);
		memmove(src + NODE_VS(node), src, (NODE_SIZE(node) - i) * NODE_VS(node));
		fnode_setval(node, i + 1, value);
	} else {
		src = fnode_getval(node, i);
		memmove(src + NODE_VS(node), src, (NODE_SIZE(node) - i) * NODE_VS(node));
		fnode_setval(node, i, value);
	}
	/*if (NODE_FLAGS(node) & BT_INTERNAL) {*/
		/*size_t __unused *t = fnode_getkey(node, i);*/
		/*bnode_ptr __unused *pt = fnode_getval(node, i + 1);*/
		/*DBUG("Node %lu - Insert Key Actual %lu -> %lu\n",node->fn_location, *t, (*pt));*/
	/*} else {*/
		/*size_t __unused *t = fnode_getkey(node, i);*/
		/*size_t __unused *pt = fnode_getval(node, i);*/
		/*size_t __unused *k = key;*/
		/*size_t __unused *p = value;*/
		/*DBUG("Insert Key attempt location %d - %lu -> %lu\n", i, *k,*p);*/
		/*DBUG("Node %lu - Insert Key Actual %lu -> %lu\n",node->fn_location, *t, *pt);*/
	/*}*/
	node->fn_dnode->dn_numkeys += 1;
}

static int
fnode_create(struct fbtree * tree, bnode_ptr ptr, struct fnode *node, int type)
{
	KASSERT(ptr != 0, ("Why are you making a blk on the allocator block"));
	int error;
	struct vnode *vp = tree->bt_backend;
	error = getptr(vp, ptr, &node->fn_buf);
	if (error) {
		DBUG("Problem with getptr\n");
		return (error);
	}
	node->fn_dnode = (struct dnode *)node->fn_buf->b_data;
	node->fn_location = ptr;
	node->fn_keys = node->fn_dnode->dn_data;
	node->fn_tree = tree;
	node->fn_max_int = MAX_NUM_INTERNAL(node);
	node->fn_max_ext = MAX_NUM_EXTERNAL(node);
	NODE_FLAGS(node) = type;

	// Node max is dependent on type to caculate correctly
	node->fn_values = (char *)node->fn_keys + (NODE_MAX(node) * NODE_KS(node));

	return (0);
}


static int
alloc_node(struct fnode *node, struct fnode *created, int type)
{
	/*bnode_ptr blkno;*/
	bnode_ptr blkno;
	int error = 0;

	struct slos *slos = (struct slos *)(node->fn_tree->bt_backend->v_data);
	blkno = slos_alloc(slos->slos_alloc, 1).offset;
	error = fnode_create(node->fn_tree, blkno, created, type);
	vfs_bio_clrbuf(created->fn_buf);


	return (error);
}

static int
fnode_internal_insert(struct fnode *node, void *key, bnode_ptr *val)
{
	void *keyt;
	int i;
	int error = 0;

	if (NODE_SIZE(node) == 0) {
		fnode_insert_at(node, key, val, 0);
		return (0);
	}

	for (i = 0; i < NODE_SIZE(node); i++) {
		keyt = fnode_getkey(node, i);
		int diff = NODE_COMPARE(node, keyt, key);
		if (diff == 0) {
			panic("Never should equal on a internal insert");
		}
		if (diff > 0) {
			break;
		}
	}

	fnode_insert_at(node, key, val, i);

	// We always allow the insert but if its max we will split 
	if (NODE_SIZE(node) == NODE_MAX(node)) {
		error = fnode_split(node);
	}

	return (error);
}

static int
fnode_newroot(struct fnode *node, struct fnode *root)
{
	struct fnode newroot;
	int error;

	struct fbtree *tree  = node->fn_tree;

	error = alloc_node(node, &newroot, BT_INTERNAL);
	if (error) {
		return (error);
	}

	newroot.fn_dnode->dn_parent = 0;
	newroot.fn_dnode->dn_numkeys = 0;
	newroot.fn_dnode->dn_flags = BT_INTERNAL;
	node->fn_dnode->dn_parent = newroot.fn_location;

	KASSERT(node->fn_dnode->dn_parent != 0, ("Should have a parent"));

	fnode_setval(&newroot, 0, &node->fn_location);
	// XXX Gotta inform the inode its attached to to change? Do we pass it a
	// function when this occurs to excute the update on it?gp

	KASSERT(node->fn_location != newroot.fn_location, ("Should be new!"));

	tree->bt_root = newroot.fn_location;
	*root = newroot;

	atomic_fetchadd_long(&node->fn_tree->bt_root_replaces, 1);

	ftree_stats(node->fn_tree);
	return (0);
}

static void
fnode_balance(struct fnode *left, struct fnode *right, size_t i)
{
	char * src;
	/* 
	 * The middle key has already been shoved into the parent by this point 
	 * so we dont need in the right side this is to hold the invariant of
	 * numKeys = numChild - 1
	 * */
	if (NODE_FLAGS(left) & BT_INTERNAL) {
		if (NODE_SIZE(left) % 2 == 0) {
			NODE_SIZE(right) = i - 1;
			NODE_SIZE(left) = i;
		} else {
			NODE_SIZE(right) = i;
			NODE_SIZE(left) = i;
		}
	} else {
		NODE_SIZE(right) = NODE_SIZE(left) - i;
		NODE_SIZE(left) = i;
	}

	if (NODE_FLAGS(left) & BT_INTERNAL) {
		src = fnode_getkey(left, i + 1);
	} else {
		src = fnode_getkey(left, i);
	}

	memcpy(fnode_getkey(right, 0), src, NODE_KS(right) * NODE_SIZE(right));

	if (NODE_FLAGS(left) & BT_INTERNAL) {
		src = fnode_getval(left, i + 1);
		memcpy(fnode_getval(right, 0), src,  sizeof(bnode_ptr) * (NODE_SIZE(right) + 1));
	} else {
		src = fnode_getval(left, i);
		memcpy(fnode_getval(right, 0), src,  NODE_VS(right) * NODE_SIZE(right));
	}
}

static int
fnode_external_insert(struct fnode *node, void *key, void *value) 
{
	void *k1;
	int i;
	int error = 0;

	for (i = 0; i < NODE_SIZE(node); i++) {
		k1 = fnode_getkey(node, i);
		int diff = NODE_COMPARE(node, k1, key);
		if (diff == 0) {
			return (EINVAL);
		}
		if (diff > 0) {
			break;
		}
	}

	fnode_insert_at(node, key, value, i);

	// We always allow the insert but if its max we will split 
	if (NODE_SIZE(node) == NODE_MAX(node)) {
		error = fnode_split(node);
	}

	return (error);
}

static int
fnode_insert(struct fnode *node, void *key, void *value) 
{
	int error;

	if (NODE_FLAGS(node) & BT_INTERNAL) {
		error = fnode_internal_insert(node, key, value);
	} else {
		error = fnode_external_insert(node, key,  value);
	}

	fnode_write(node);
	return (error);
}

/* [0 (less than 5)] 5 [1( greater than 5 by less than 10] 10 [2] 15 [3]
 * Find element thats greater than it than take that index
 * */
static int
fnode_split(struct fnode *node) 
{
	struct fnode right;
	struct fnode parent;
	int error;

	// This signifies the root node, if this is the case we need handle
	// specifically as we need 2 node allocations specifically
	if (node->fn_location == node->fn_tree->bt_root) {
		error = fnode_newroot(node, &parent);
		if (error) {
		    DBUG("ERROR CREATING NEW ROOT\n");
		    return (error);
		}
		KASSERT(node->fn_location != node->fn_tree->bt_root, ("SHOULD BE DIFFERENT NOW"));

	} else {
		error = fnode_init(node->fn_tree, node->fn_dnode->dn_parent, &parent);
		if (error) {
			return (error);
		}
		KASSERT(NODE_FLAGS(&parent) & BT_INTERNAL, ("Must be an internal node to be a parent"));
	}

	size_t mid_i = NODE_SIZE(node) / 2;
	error = alloc_node(node, &right, NODE_FLAGS(node));
	if (error) {
		return (error);
	}

	node->fn_dnode->dn_parent = parent.fn_location;
	right.fn_dnode->dn_parent = node->fn_dnode->dn_parent;

	KASSERT(right.fn_location != 0, ("Why did this get allocated here"));
	fnode_insert(&parent, fnode_getkey(node, mid_i), &right.fn_location);

	right.fn_dnode->dn_rightnode = node->fn_dnode->dn_rightnode;
	node->fn_dnode->dn_rightnode = right.fn_location;

	KASSERT(node->fn_dnode->dn_parent != 0, ("Should have a parent now\n"));
	KASSERT(right.fn_dnode->dn_parent != 0, ("Should have a parent now\n"));
	KASSERT(parent.fn_dnode->dn_flags == BT_INTERNAL, ("Should be an internal node\n"));

	fnode_balance(node, &right, mid_i);

	// Don't need to write the left node, as once it pops from insert it 
	// will be dirtied.
	fnode_write(&right);

	atomic_fetchadd_long(&node->fn_tree->bt_splits, 1);

	return (0);
}


int 
fbtree_insert(struct fbtree *tree, void *key, void *value)
{
	int error;
	struct fnode node;

	rw_wlock(&tree->bt_lock);

	error = fnode_init(tree, tree->bt_root, &node);
	if (error) {
		rw_wunlock(&tree->bt_lock);
		return (error);
	}

	error = fnode_follow(&node, key);
	if (error) {
		DBUG("Problem following node\n");
		rw_wunlock(&tree->bt_lock);
		return (error);
	}

	error = fnode_insert(&node, key, value);
	if (error) {
	    rw_wunlock(&tree->bt_lock);
	    return (error);
	}

	rw_wunlock(&tree->bt_lock);
	atomic_fetchadd_long(&tree->bt_inserts, 1);

	return (0);
}

int 
fbtree_replace(struct fbtree *tree, void *key, void *value)
{
	struct fnode node;
	struct fnode_iter iter;
	int error;

	rw_wlock(&tree->bt_lock);

	error = fnode_init(tree, tree->bt_root, &node);
	if (error) {
		rw_wunlock(&tree->bt_lock);
		return (error);
	}

	error = fnode_keymin_iter(&node, key, &iter);
	if (error) {
		DBUG("Never found\n");
		rw_wunlock(&tree->bt_lock);
		return (error);
	}

	if(NODE_COMPARE(&node, key, ITER_KEY(iter)) != 0) {
		return (-1);
	}

	memcpy(ITER_VAL(iter), value, tree->bt_valsize);

	fnode_write(&iter.it_node);

	rw_wunlock(&tree->bt_lock);

	atomic_fetchadd_long(&tree->bt_replaces, 1);
	return (0);
}


int 
fnode_init(struct fbtree *tree, bnode_ptr ptr, struct fnode *node)
{
	KASSERT(ptr != 0, ("Why are you making a blk on the allocator block"));
	int error;
	struct vnode *vp = tree->bt_backend;

	error = getptr(vp, ptr, &node->fn_buf);
	if (error) {
		return (error);
	}

	node->fn_dnode = (struct dnode *)node->fn_buf->b_data;
	node->fn_location = ptr;
	node->fn_keys = node->fn_dnode->dn_data;
	node->fn_tree = tree;
	node->fn_max_int = MAX_NUM_INTERNAL(node);
	node->fn_max_ext = MAX_NUM_EXTERNAL(node);
	node->fn_values = (char *)node->fn_keys + (NODE_MAX(node) * NODE_KS(node));

	return (0);
}

int 
fbtree_test(struct fbtree *tree)
{
	int error;
	int size = 4096 * 2;
	size_t  *keys = (size_t *)malloc(sizeof(size_t) * size, M_SLOS, M_WAITOK);
	srandom(1);
	for (int i = 0; i < size; i++) {
		keys[i] = random();
		size_t bigboi = i;
		error = fbtree_insert(tree, &keys[i], &bigboi);
		if (error) {
			DBUG("%d ERROR\n", error);
		}
		for (int t = 0; t < i; t++) {
			size_t bigboi;
			error = fbtree_get(tree, &keys[t], &bigboi);
			if (error) {
				DBUG("%d ERROR\n", error);
				DBUG("KEY %lu - %lu", keys[t], keys[t]);
				panic("Problem");
			}
			KASSERT(bigboi == t, ("Test not equal"));
		}
	}

	free(keys, M_SLOS);

	return (0);
}

