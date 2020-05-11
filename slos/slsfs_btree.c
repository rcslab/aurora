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
#include <sys/vnode.h>
#include <sys/rwlock.h>
#include <machine/atomic.h>

#include "slosmm.h"
#include "slos.h"
#include "slos_inode.h"
#include "slsfs_alloc.h"
#include "slsfs_buf.h"

#define NODE_LOCK(node, flags) (BUF_LOCK((node)->fn_buf, flags, NULL))
#define NODE_ISLOCKED(node) (BUF_ISLOCKED((node)->fn_buf))

uma_zone_t fnode_zone;

static void
fnode_alloc(void **node)
{
	*node = uma_zalloc(fnode_zone, 0);
	((struct fnode *)(*node))->fn_inited = 0;
	((struct fnode *)(*node))->fn_right = NULL;
	((struct fnode *)(*node))->fn_parent = NULL;
	((struct fnode *)(*node))->fn_dnode = NULL;
	bzero(((struct fnode *)(*node))->fn_pointers, sizeof(void *) * 300);
}

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

/*
 * Write out a tree node to disk.
 */
void
fnode_write(struct fnode *node)
{
	struct fbtree *tree = node->fn_tree;
	struct buflist *bl;

	int found = 0;
	/**
	 * This is a feature currently not used - but didn't want to take it
	 * out just yet.  This gives us the ability to track each dirty buf of
	 * a specific btree.  This is because btrees are usuaully backed by the
	 * same vnode backend.
	 */
	if (tree->bt_flags & FBT_TRACKBUF) {
		LIST_FOREACH(bl, &tree->bt_dirtybuf, l_entry) {
			if (bl->l_buf == node->fn_buf) {
				found = 1;
				break;
			}
		}

		if (!found) {
			bl = malloc(sizeof(struct buflist), M_SLOS, M_WAITOK);
			bl->l_buf = node->fn_buf;
			LIST_INSERT_HEAD(&tree->bt_dirtybuf, bl, l_entry);
			tree->bt_dirtybuf_cnt++;
		}
	}
}

/*
 * Getters and setters for the keys and values of the btree. The type argument
 * is a key as to whether the node is internal or external.
 */
void *
fnode_getkey(struct fnode *node, int i)
{
	KASSERT(i <= NODE_MAX(node), ("Not getting key past max - %p - %d - %ld", node, i, NODE_MAX(node)));
	KASSERT(node->fn_keys != NULL, ("Use fnode INIT"));
	return  (char *)node->fn_keys + (i * node->fn_tree->bt_keysize);
}

void *
fnode_getval(struct fnode *node, int i)
{
	KASSERT(i <= NODE_MAX(node), ("Not getting val past max - %p - %d", node,  i));
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
	KASSERT(i < NODE_MAX(node), ("Not inserting val past max - %p", node));
	memcpy(fnode_getval(node, i), val, NODE_VS(node));
}

void
fnode_print(struct fnode *node)
{
	int i;
	printf("%s - NODE: %lu, rightnode: %lu\n", node->fn_tree->bt_name, node->fn_location, node->fn_dnode->dn_rightnode);
	if (NODE_FLAGS(node) & BT_INTERNAL) {
		bnode_ptr *p = fnode_getval(node, 0);
		printf("| C %lu |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = fnode_getval(node, i + 1);
			uint64_t __unused *t = fnode_getkey(node, i);
			printf("| K %lu || C %lu |", *t, *p);
		}
	} else {
		for (i = 0; i < NODE_SIZE(node); i++) {
			uint64_t __unused *t = fnode_getkey(node, i);
			diskptr_t __unused *v = fnode_getval(node, i);
			printf("| %lu -> %lu, %lu |,", *t, v->offset, v->size);
		}
	}
	if (!node->fn_right) {
		fnode_right(node, &node->fn_right);
	}
	printf("\n");
	if (node->fn_right != NULL) {
		fnode_print(node->fn_right);
	}
}

/*
 * Get the buffer for an a btree node.
 *
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
fnode_getbufptr(struct vnode *vp, bnode_ptr ptr, struct buf **bp)
{
	struct buf *buf;
	int error;

	struct slos *slos = (struct slos *)vp->v_data;

	CTR2(KTR_SPARE5, "fnode_getbufptr vp=%p ptr=%d", vp, ptr);
	KASSERT(ptr != 0, ("Should never be 0"));
	error = bread(vp, ptr, slos->slos_sb->sb_bsize, curthread->td_ucred, &buf);
	if (error) {
		DBUG("Error reading block\n");
		return (EIO);
	}

	*bp = buf;

	return (0);
}

int
fnode_fetch(struct fnode *node, int index, struct fnode **next)
{
	bnode_ptr ptr;
	if (node->fn_pointers[index] == NULL) {
		ptr = *(bnode_ptr *)fnode_getval(node, index);
		if (ptr == 0) {
			printf("%d index\n", index);
			fnode_print(node);
			panic("Should not be zero");
		}
		fnode_init(node->fn_tree, ptr, (struct fnode **)&node->fn_pointers[index]);
	} 

	*next = (struct fnode *)node->fn_pointers[index];
	return (0);
}

/*
 * Find the only possible external bnode that can contain the key.
 */
static int
fnode_follow(struct fnode *root, const void *key, struct fnode **node)
{
	int error = 0;
	void *keyt;
	struct fnode *next;
	struct fnode *cur;
	int index, start, end, mid, compare;

	cur = root;

	while (NODE_FLAGS(cur) & BT_INTERNAL) {
		// Linear search for now
		start = 0;
		end = NODE_SIZE(cur) - 1;
		if (NODE_SIZE(cur)) {
			index = -1;
			while (start <= end) {
				mid = (start + end) / 2;
				keyt = fnode_getkey(cur, mid);
				compare = NODE_COMPARE(cur, keyt, key);
				if (compare <= 0) {
					start = mid + 1;
				} else {
					index = mid;
					end = mid - 1;
				}
			}

			if (index == (-1)) {
				index = NODE_SIZE(cur) - 1;
			}
		} else {
			index = 0;
		}

		if (NODE_COMPARE(cur, fnode_getkey(cur, index), key) < 0) {
			index++;
		}
		/* Create an in-memory copy of the next bnode in the path. */
		fnode_fetch(cur, index, &next);
		if (error != 0) {
			break;
		}

		/*
		 * Lazily fix up erroneous parent pointers. This is
		 * needed because we don't fix them when splitting
		 * and merging internal nodes.
		 */
		if (next->fn_dnode->dn_parent != cur->fn_location) {
			next->fn_dnode->dn_parent = cur->fn_location;
			next->fn_parent = cur;
			fnode_write(next);
		}

		/* Prepare to traverse the next node. */
		cur = next;
	}

	/* XXX This is going to trigger if we get an error in fnode_init */

	*node = cur;
	KASSERT((NODE_FLAGS(*node) & BT_INTERNAL) == 0, ("Node still internal\n"));

	return (error);
}


/*
 * Initialize an in-memory btree.
 */
int
fbtree_init(struct vnode *backend, bnode_ptr ptr, fb_keysize keysize,
    fb_valsize valsize, compare_t comp, char * name, uint64_t flags, struct fbtree *tree)
{
	tree->bt_backend = backend;
	tree->bt_keysize = keysize;
	tree->bt_valsize = valsize;
	tree->comp = comp;
	tree->bt_root = ptr;
	tree->bt_flags = flags;
	strcpy(tree->bt_name, name);
	tree->bt_dirtybuf_cnt = 0;
	bzero(&tree->bt_dirtybuf, sizeof(tree->bt_dirtybuf));
	tree->bt_rootnode = NULL;

	if (!lock_initialized(&tree->bt_lock.lock_object)) {
		lockinit(&tree->bt_lock, PVFS, "Btree Lock", 0, 0);
	}

	return (0);
}

int
fnode_right(struct fnode *node, struct fnode **right)
{
	if (node->fn_dnode->dn_rightnode == 0) {
		*right = NULL;
		return (0);
	}

	fnode_init(node->fn_tree, node->fn_dnode->dn_rightnode, &node->fn_right);
	*right = node->fn_right;
	return (0);
}

/*
 * Get the total amount of keys in a btree.
 */
size_t
fbtree_size(struct fbtree *tree)
{
	int error;
	size_t key = 0;
	size_t size = 0;

	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		return (error);
	}

	struct fnode *node;
	error = fnode_follow(tree->bt_rootnode, &key, &node);
	if (error) {
		DBUG("Problem following node\n");
		return (error);
	}

	/* Move to the right, counting how many keys we come across. */
	struct fnode *right = NULL;
	while(node->fn_dnode->dn_rightnode != 0) {
		size += node->fn_dnode->dn_numkeys;
		fnode_right(node, &right);
		if (right == NULL) {
			break;
		}
		node = right;
	};

	size += node->fn_dnode->dn_numkeys;

	return size;
}

/*
 * Find the location of the largest key smaller than or equal to the one provided.
 */
static int
fnode_keymin_iter(struct fnode *root, void *key, struct fnode_iter *iter)
{
	int error;
	void *val;
	int diff;
	int mid;

	struct fnode *node;

	// Follow catches if this is an external node or not
	error = fnode_follow(root, key, &node);
	if (error) {
		return (error);
	}

	/* If the tree is empty, there is no minimum. */
	if (NODE_SIZE(node) == 0) {
		iter->it_index = -1;
		iter->it_node = node;
		return (0);
	}

	KASSERT((NODE_FLAGS(node) & (BT_INTERNAL)) == 0, ("Should be on a external node now"));

	/* Traverse the node to find the infimum. */
	for(mid = 0; mid < NODE_SIZE(node); mid++) {
		val = fnode_getkey(node, mid);
		diff = NODE_COMPARE(node, val, key);
		if (diff > 0) {
			mid--;
			break;
		}
		if (diff == 0) {
			break;
		}
	}

	iter->it_node = node;
	iter->it_index = mid;

	/* If we went out of bounds, there is no infimum for the key. */
	if ((mid == NODE_SIZE(node)) || (mid < 0))
		iter->it_index = -1;

	return (0);
}

/*
 * Find the largest key in the subtree that's smaller than or equal to the key provided.
 */
int
fnode_keymin(struct fnode *root, void *key, void *value)
{
	int error;
	struct fnode_iter iter;

	/* Get the location of the minimum key-value pair. */
	error = fnode_keymin_iter(root, key, &iter);
	if (error) {
		return (error);
	}

	KASSERT(iter.it_index != (-1), ("Should find something - %lu - %p", *(uint64_t *)key, &iter.it_node));

	/* Copy the pair out and release the node buffer. */
	memcpy(value, ITER_VAL(iter), root->fn_tree->bt_valsize);
	memcpy(key, ITER_KEY(iter), root->fn_tree->bt_keysize);

	return (0);
}

/*
 * Find the largest key in the subtree that's smaller than the key provided.
 */
int
fbtree_keymin_iter(struct fbtree *tree, void *key, struct fnode_iter *iter)
{
	int error;

	/* Bootstrap the search process by initializing the in-memory node. */
	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		DBUG("Problem initing\n");
		BTREE_UNLOCK(tree, 0);
		return (error);
	}

	error = fnode_keymin_iter(tree->bt_rootnode, key, iter);
	return (error);
}

/*
 * Find the smallest key in the subtree that's larger than or equal to the key provided.
 */
static int
fnode_keymax_iter(struct fnode *root, void *key, struct fnode_iter *iter)
{
	int error;
	void *val;
	int diff;
	int mid;
	struct fnode *node;

	// Follow catches if this is an external node or not
	error = fnode_follow(root, key, &node);
	if (error) {
		return (error);
	}

	/* If the tree is empty, there is no maximum. */
	if (NODE_SIZE(node) == 0) {
		return (EINVAL);
	}

	/* Traverse the node to find the supremum. */
	KASSERT((NODE_FLAGS(node) & (BT_INTERNAL)) == 0, ("Should be on a external node now"));
	for(mid = 0; mid < NODE_SIZE(node); mid++) {
		val = fnode_getkey(node, mid);
		diff = NODE_COMPARE(node, val, key);
		if (diff >= 0) {
			break;
		}
	}

	iter->it_node = node;
	iter->it_index = mid;

	/* If we went out of bounds, there is no supremum for the key. */
	if ((mid == NODE_SIZE(node)) || (mid < 0))
		iter->it_index = -1;

	return (0);
}

/*
 * Find the smallest key in the tree that's larger than or equal to the key provided.
 */
int
fbtree_keymax_iter(struct fbtree *tree, void *key, struct fnode_iter *iter)
{
	int error;

	/* Bootstrap the search process by initializing the in-memory node. */
	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		BTREE_UNLOCK(tree, 0);
		return (error);
	}

	error = fnode_keymax_iter(tree->bt_rootnode, key, iter);
	return (error);
}

/*
 *  Iterate through the keys of the btree, traversing external bnodes if necessary.
 */
int
fnode_iter_next(struct fnode_iter *it)
{
	struct dnode *dn = it->it_node->fn_dnode;
	bnode_ptr ptr;

	/* An invalid iterator is still invalid after iteration . */
	if (it->it_index == (-1)) {
		return (-1);
	}

	/* If we've exhausted the current node, switch. */
	if (it->it_index == dn->dn_numkeys) {
		/* Iteration done. */
		if (dn->dn_rightnode == 0) {
			it->it_index = -1;
			return (-1);
		}

		/* Otherwise switch nodes. */
		ptr = dn->dn_rightnode;
		fnode_right(it->it_node, &it->it_node);
		it->it_index = 0;
		return (0);
	}

	/* Going out of bounds in the node would be catastrophic. */
	if (it->it_index >= dn->dn_numkeys)
		panic("Should not occur");

	it->it_index++;
	return (0);
};

/*
 * Search a key-value pair in the btree, reaturn it if found.
 */
int
fbtree_get(struct fbtree *tree, const void *key, void *value)
{
	char check[tree->bt_keysize];
	int error;

	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		BTREE_UNLOCK(tree, 0);
		return (error);
	}

	memcpy(check, key, tree->bt_keysize);

	/*
	 * A keymin operation does a superset of the
	 * work a search does, so we reuse the logic.
	 */
	error = fnode_keymin(tree->bt_rootnode, check, value);
	if (error) {
		return (error);
	}

	/* Check if the keymin returned the key. */
	if (NODE_COMPARE(tree->bt_rootnode, key, check)) {
		value = NULL;
		return (EINVAL);
	}

	return (0);
}

/*
 * Create a node for the btree at the specified offset.
 */
static int
fnode_create(struct fbtree *tree, bnode_ptr ptr, struct fnode *node, int type)
{
	int error;
	struct vnode *vp = tree->bt_backend;

	KASSERT(ptr != 0, ("Why are you making a blk on the allocator block"));

	/* Get the new bnode's block into the cache. */
	error = fnode_getbufptr(vp, ptr, &node->fn_buf);
	if (error) {
		DBUG("Problem with fnode_getbufptr\n");
		return (error);
	}
	node->fn_dnode = (struct dnode *)node->fn_buf->b_data;
	node->fn_location = ptr;
	node->fn_keys = node->fn_dnode->dn_data;
	node->fn_tree = tree;
	node->fn_max_int = MAX_NUM_INTERNAL(node);
	node->fn_max_ext = MAX_NUM_EXTERNAL(node);
	node->fn_dnode->dn_flags = type;
	node->fn_inited = 1;

	// Node max is dependent on type to calculate correctly
	node->fn_values = (char *)node->fn_keys + (NODE_MAX(node) * NODE_KS(node));
	node->fn_buf->b_flags |= B_MANAGED;
	bqrelse(node->fn_buf);

	return (0);
}


/*
 * Allocate a new for the tree, along with any necessary on-disk resources.
 */
static int
fbtree_allocnode(struct fbtree *tree, struct fnode *created, int type)
{
	diskptr_t ptr;
	int error = 0;

	struct slos *slos = (struct slos *)(tree->bt_backend->v_data);
	error = ALLOCATEBLK(slos, BLKSIZE(slos), &ptr);
	if (error) {
		return (error);
	}

	error = fnode_create(tree, ptr.offset, created, type);
	if (error != 0) {
		return (error);
	}

	bzero(created->fn_buf->b_data, created->fn_buf->b_bcount);

	return (0);
}

/*
 * Insert a key-value pair into a node at the specified offset.
 */
static void
fnode_insert_at(struct fnode *node, void *key, void *value, int i)
{
	char *src;

	if (i >= node->fn_max_int) {
		fnode_print(node);
	}
	KASSERT(i < node->fn_max_int, ("Insert is out of bounds at %d", i));

	/* The keys scoot over to make room for the new one. */
	src = fnode_getkey(node, i);
	memmove(src + node->fn_tree->bt_keysize, src, (NODE_SIZE(node) - i) * NODE_KS(node));
	fnode_setkey(node, i, NODE_FLAGS(node), key);
	node->fn_dnode->dn_numkeys += 1;

	/* The offset of the value in the node depends if it's internal or external. */
	if (NODE_FLAGS(node) & BT_INTERNAL) {
		src = fnode_getval(node, i + 1);
		memmove(src + NODE_VS(node), src, (NODE_SIZE(node) - i) * NODE_VS(node));
		fnode_setval(node, i + 1, value);
	} else {
		src = fnode_getval(node, i);
		memmove(src + NODE_VS(node), src, (NODE_SIZE(node) - i) * NODE_VS(node));
		fnode_setval(node, i, value);
	}
}

/*
 * Assign a new root to a btree whose root is the given node.
 */
static int
fnode_newroot(struct fnode *node, struct fnode **root)
{
	struct fnode *newroot;
	struct fbtree *tree  = node->fn_tree;
	int error;

	KASSERT(tree->bt_root == node->fn_location, ("node is not the root of its btree"));
	/* Allocate a new node. */
	fnode_alloc((void **)&newroot);
	error = fbtree_allocnode(node->fn_tree, newroot, BT_INTERNAL);
	if (error) {
		return (error);
	}

	newroot->fn_dnode->dn_parent = 0;
	newroot->fn_dnode->dn_numkeys = 0;
	newroot->fn_dnode->dn_flags = BT_INTERNAL;
	node->fn_dnode->dn_parent = newroot->fn_location;
	node->fn_parent = newroot;

	KASSERT(node->fn_dnode->dn_parent != 0, ("Should have a parent"));

	/*
	 * The new root's child is the old root. This makes it imbalanced,
	 * but the caller should fix it up.
	 */
	fnode_setval(newroot, 0, &node->fn_location);

	// XXX Gotta inform the inode its attached to to change? Do we pass it a
	// function when this occurs to execute the update on it?

	KASSERT(node->fn_location != newroot->fn_location, ("Should be new!"));

	/* Update the tree. */
	tree->bt_root = newroot->fn_location;
	tree->bt_rootnode = newroot;
	*root = newroot;

	ftree_stats(node->fn_tree);

	return (0);
}

/*
 * Rebalance the keys of two nodes between them. Right now it's assumed
 * that the left one is full and the right one is empty.
 */
static void
fnode_balance(struct fnode *left, struct fnode *right, size_t i)
{
	char *src;

	/*
	 * Adjust the node sizes. For the left node, this basically throws out half the nodes. *
	 *
	 * The middle key has already been shoved into the parent by this point
	 * so we dont need in the right side this is to hold the invariant of
	 * numKeys = numChild - 1.
	 */
	NODE_FLAGS(right) = NODE_FLAGS(left);
	if (NODE_FLAGS(left) & BT_INTERNAL) {
		/*
		 * The size of the node i is the number of its keys. By splitting an
		 * internal node, we are removing a key from both resulting nodes and
		 * putting it on their common parent, so their total size is one less
		 * than that of the original.
		 */
		NODE_SIZE(right) = NODE_SIZE(left) - i - 1;
		NODE_SIZE(left) = i;
	} else {
		/* Split all key-value pairs among the two resulting nodes. */
		NODE_SIZE(right) = NODE_SIZE(left) - i;
		NODE_SIZE(left) = i;
	}

	/* Get a pointer to the key subarray that we will transfer to the right node. */
	if (NODE_FLAGS(left) & BT_INTERNAL) {
		/* Skip key i, we'll put it in the parent node. */
		src = fnode_getkey(left, i + 1);
	} else {
		src = fnode_getkey(left, i);
	}

	/* Copy over keys array from the left node the right. */
	memcpy(fnode_getkey(right, 0), src, NODE_KS(right) * NODE_SIZE(right));

	/* Copy over the values. */
	if (NODE_FLAGS(left) & BT_INTERNAL) {
		/* Value i stays with the left node, so start from value i + 1. */
		src = fnode_getval(left, i + 1);
		memcpy(fnode_getval(right, 0), src,  sizeof(bnode_ptr) * (NODE_SIZE(right) + 1));
	} else {
		src = fnode_getval(left, i);
		memcpy(fnode_getval(right, 0), src,  NODE_VS(right) * NODE_SIZE(right));
	}
}

static void
fnode_setparent(struct fnode *node, struct fnode *parent)
{
	node->fn_dnode->dn_parent = parent->fn_location;
	node->fn_parent = parent;
}

int
fnode_parent(struct fnode *node, struct fnode **parent)
{
	if (!node->fn_parent) {
		fnode_init(node->fn_tree, node->fn_dnode->dn_parent, &node->fn_parent);
	}
	*parent = node->fn_parent;
	return (0);
}


static void
fnode_setright(struct fnode *node, struct fnode *right)
{
	node->fn_dnode->dn_rightnode = right->fn_location;
	node->fn_right = right;
}

/* [0 (less than 5)] 5 [1( greater than 5 by less than 10] 10 [2] 15 [3]
 * Find element thats greater than it than take that index
 * */
static int
fnode_split(struct fnode *node)
{
	struct fnode *right;
	struct fnode *parent = NULL;
	int error;
	size_t mid_i;

	// This signifies the root node, if this is the case we need handle
	// specifically as we need 2 node allocations specifically
	/* Make the left node. If we're splitting the root we need extra bookkeeping */
	if (node->fn_location == node->fn_tree->bt_root) {
		error = fnode_newroot(node, &parent);
		if (error) {
			DBUG("ERROR CREATING NEW ROOT\n");
			return (error);
		}
		KASSERT(node->fn_location != node->fn_tree->bt_root, ("Problem with fnode - %p", node));

	}  else {
		fnode_parent(node, &parent);
	}

	/* Make the right node. */
	fnode_alloc((void **)&right);
	error = fbtree_allocnode(node->fn_tree, right, NODE_FLAGS(node));
	if (error) {
		return (error);
	}

	/* Adjust parent and neighbor pointers. */
	DBUG("set parent node\n");
	fnode_setparent(node, parent);
	DBUG("set parent right\n");
	fnode_setparent(right, parent);

	right->fn_dnode->dn_rightnode = node->fn_dnode->dn_rightnode;
	right->fn_right = node->fn_right;

	fnode_setright(node, right);

	KASSERT(right->fn_location != 0, ("Why did this get allocated here"));
	KASSERT(node->fn_dnode->dn_parent != 0, ("Should have a parent now\n"));
	KASSERT(right->fn_dnode->dn_parent != 0, ("Should have a parent now\n"));
	KASSERT(parent->fn_dnode->dn_flags == BT_INTERNAL, ("Should be an internal node\n"));

	/*
	 * Redistribute the original node's contents to the two new ones. We
	 * split the keys equally.
	 */
	mid_i = NODE_SIZE(node) / 2;
	fnode_balance(node, right, mid_i);

	// Don't need to write the left node, as once it pops from insert it
	// will be dirtied.
	/*
	 * Insert the new node to the parent. If the parent is
	 * at capacity, this will cause a split it.
	 *
	 * This is indirectly recursive, but btrees are shallow so there is no problem
	 */
	error = fnode_insert(parent, fnode_getkey(node, mid_i), &right->fn_location);
	fnode_write(right);

	return (0);
}

/*
 * Insert a key-value pair into an internal node.
 */
static int
fnode_internal_insert(struct fnode *node, void *key, bnode_ptr *val)
{
	void *keyt;
	int error = 0;
	int start, end, index, mid;
	int compare;

	/* Find the proper position in the bnode to insert. */
	start = 0;
	end = NODE_SIZE(node) - 1;
	if (NODE_SIZE(node)) {
		index = -1;
		while (start <= end) {
			mid = (start + end) / 2;
			keyt = fnode_getkey(node, mid);
			compare = NODE_COMPARE(node, keyt, key);
			if (compare <= 0) {
				start = mid + 1;
			} else {
				index = mid;
				end = mid - 1;
			}
		}

		if (index == (-1)) {
			index = NODE_SIZE(node);
		}
	} else {
		index = 0;
	}

	fnode_insert_at(node, key, val, index);

	/* Split the internal node if over capacity. */
	if ((NODE_SIZE(node) + 1) == NODE_MAX(node)) {
		error = fnode_split(node);
		if (error) {
			return (error);
		}

		return (INTERNALSPLIT);
	}

	return (error);
}

/*
 * Insert a key-value pair into an external btree node.
 */
static int
fnode_external_insert(struct fnode *node, void *key, void *value)
{
	void *keyt;
	int error = 0;
	int start, end, index, compare, mid;

	start = 0;
	end = NODE_SIZE(node) - 1;
	if (NODE_SIZE(node)) {
		index = -1;
		while (start <= end) {
			mid = (start + end) / 2;
			keyt = fnode_getkey(node, mid);
			compare = NODE_COMPARE(node, keyt, key);
			if (compare <= 0) {
				start = mid + 1;
			} else {
				index = mid;
				end = mid - 1;
			}
		}

		if (index == (-1)) {
			index = NODE_SIZE(node);
		}
	} else {
		index = 0;
	}

	fnode_insert_at(node, key, value, index);

	// We always allow the insert but if its max we will split
	if ((NODE_SIZE(node) + 1) == NODE_MAX(node)) {
		error = fnode_split(node);
	}

	return (error);
}

/*
 * Insert a key-value pair into a btree node.
 */
int
fnode_insert(struct fnode *node, void *key, void *value)
{
	int error;

	bnode_ptr ptr = node->fn_tree->bt_root;
	if (NODE_FLAGS(node) & BT_INTERNAL) {
		error = fnode_internal_insert(node, key, value);
	} else {
		error = fnode_external_insert(node, key,  value);
	}

	/* If we changed the root, start */
	if (ptr != node->fn_tree->bt_root) {
		error = ROOTCHANGE;
	}
	fnode_write(node);

	return (error);
}

/*
 *  Remove a key-value pair from a node.
 */
static int
fnode_remove_at(struct fnode *node, void *key, void *value, int i)
{
	char *src = fnode_getkey(node, i);

	/* Scoot over the keys to the left, overwriting the one in position i.*/
	memmove(src, src + node->fn_tree->bt_keysize, (NODE_SIZE(node) - i) * NODE_KS(node));

	/* The offset of the corresponding value is different depending if the node is internal or not. */
	if (NODE_FLAGS(node) & BT_INTERNAL) {
		src = fnode_getval(node, i + 1);
		memmove(src, src + NODE_VS(node), (NODE_SIZE(node) - i) * NODE_VS(node));
	} else {
		src = fnode_getval(node, i);
		memcpy(value, src, node->fn_tree->bt_valsize);
		memmove(src, src + NODE_VS(node), (NODE_SIZE(node) - i) * NODE_VS(node));
	}
	node->fn_dnode->dn_numkeys -= 1;

	return (0);
}

/*
 * Merge two nodes together.
 *
 * XXX Implement (the signature also needs to be changed,
 * it should be an internal bnode and an offset.
 */
static int
fnode_merge(struct fnode *node)
{
	panic("Not Implemented");
	return (0);
}

/*
 * Remove a key-value pair from an internal node.
 * XXX Implement
 */
static int
fnode_internal_remove(struct fnode *node, void *key, bnode_ptr *val)
{
	return (ENOSYS);
}

/*
 * Remove a key-value pair from an external node.
 */
static int
fnode_external_remove(struct fnode *node, void *key, void *value)
{
	void *k1;
	int i;
	int error = 0;
	int isroot;
	int diff;

	/* Do a linear search for the key to be removed. */
	for (i = 0; i < NODE_SIZE(node); i++) {
		k1 = fnode_getkey(node, i);
		diff = NODE_COMPARE(node, k1, key);
		if (diff == 0) {
			break;
		}
		if (diff > 0) {
			return (EINVAL);
		}
	}

	/* Actually remove the element. */
	fnode_remove_at(node, key, value, i);

	// We always allow the insert but if its max we will split
	isroot = node->fn_location == node->fn_tree->bt_root;
	if ((NODE_SIZE(node) < ((NODE_MAX(node) / 2) - 1)) && !isroot) {
		error = fnode_merge(node);
	}

	return (error);
}

/*
 * Remove a key-value pair from a node.
 */
static int
fnode_remove(struct fnode *node, void *key, void *value)
{
	bnode_ptr ptr = node->fn_tree->bt_root;
	int error;

	if (NODE_FLAGS(node) & BT_INTERNAL) {
		error = fnode_internal_remove(node, key, value);
	} else {
		error = fnode_external_remove(node, key,  value);
	}

	/* Write out the node. */
	fnode_write(node);

	/* Check if the root changed. */
	if (ptr != node->fn_tree->bt_root) {
		return ROOTCHANGE;
	}
	return (error);
}

/*
 * Safely remove a key-value pair while iterating over it.
 *
 * XXX Implement properly
 */
int
fiter_remove(struct fnode_iter *it)
{
	int error;
	struct fbtree *tree = it->it_node->fn_tree;
	char check[tree->bt_valsize];

	error = fnode_remove(it->it_node, ITER_KEY(*it), check);
	return (error);
}

/*
 * Remove a key-value pair from the btree, if present.
 */
int
fbtree_remove(struct fbtree *tree, void *key, void *value)
{
	int error;
	struct fnode *node;

	/* Get an in-memory representation of the root. */
	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		return (error);
	}

	/* Find the only possible location of the key. */
	error = fnode_follow(tree->bt_rootnode, key, &node);
	if (error) {
		return (error);
	}

	/* Remove it from the node. */
	error = fnode_remove(node, key, value);
	if (error != ROOTCHANGE) {
		return (error);
	}

	/* We propagate a possible root change to the caller. */
	return (error);
}


/*
 * Insert a key-value pair to the tree, if not already present.
 */
int
fbtree_insert(struct fbtree *tree, void *key, void *value)
{
	int error;
	struct fnode *node;

	/* Get an in-memory representation of the root. */
	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		return (error);
	}

	/* Find the only possible location where we can add the key. */
	error = fnode_follow(tree->bt_rootnode, key, &node);
	if (error) {
		DBUG("Problem following node\n");
		return (error);
	}

	/* Add it to the node. */
	error = fnode_insert(node, key, value);
	if (error != ROOTCHANGE) {
		return (error);
	}

	/* We propagate a possible root change to the caller. */
	return (error);
}


/*
 * Replace a the value of a key in the tree, if already present.
 */
int
fbtree_replace(struct fbtree *tree, void *key, void *value)
{
	struct fnode_iter iter;
	int error;

	/* Get an in-memory representation of the root. */
	error = fnode_init(tree, tree->bt_root, &tree->bt_rootnode);
	if (error) {
		return (error);
	}

	/* Find the only possible location of the key. */
	error = fnode_keymin_iter(tree->bt_rootnode, key, &iter);
	if (error) {
		return (error);
	}

	/* Make sure the key is present, and overwrite the value in place. */
	if(NODE_COMPARE(tree->bt_rootnode, key, ITER_KEY(iter)) != 0) {
		panic("%p, %lu", iter.it_node, *(uint64_t *)key);
		return EINVAL;
	}

	memcpy(ITER_VAL(iter), value, tree->bt_valsize);

	fnode_write(iter.it_node);

	return (0);
}


/*
 * Create an in-memory representation of a bnode.
 */
int
fnode_init(struct fbtree *tree, bnode_ptr ptr, struct fnode **fn)
{
	int error;
	struct vnode *vp = tree->bt_backend;
	struct fnode *node;

	if (*fn == NULL) {
		fnode_alloc((void **)&node);
	} else {
	    node = *fn;
	}

	if (node->fn_dnode) {
		return (0);
	}

	CTR2(KTR_SPARE5, "fnode_init vp=%p ptr=%d", vp, ptr);
	/* Read the data from the disk into the buffer cache. */
	error = fnode_getbufptr(vp, ptr, &node->fn_buf);
	if (error) {
		*fn = NULL;
		return (error);
	}

	node->fn_dnode = (struct dnode *)node->fn_buf->b_data;
	node->fn_location = ptr;
	node->fn_keys = node->fn_dnode->dn_data;
	node->fn_tree = tree;
	node->fn_max_int = MAX_NUM_INTERNAL(node);
	node->fn_max_ext = MAX_NUM_EXTERNAL(node);
	node->fn_values = (char *)node->fn_keys + (NODE_MAX(node) * NODE_KS(node));
	node->fn_inited = 1;

	*fn = node;

	node->fn_buf->b_flags |= B_MANAGED;
	bqrelse(node->fn_buf);

	return (0);
}

int
fbtree_test(struct fbtree *tree)
{
	int error;
	int size = 10000 * 2;
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

#ifdef DDB
#include "opt_ddb.h"
#include <ddb/ddb.h>

DB_SHOW_COMMAND(fnode, db_fnode)
{
	if (!have_addr) {
		db_printf("usage: fnode addr");
	}

	struct fnode *node = (struct fnode *)addr;
	int i = 0;

	db_printf("%s - NODE: %lu\n", node->fn_tree->bt_name, node->fn_location);
	db_printf("Size : %u\n", node->fn_dnode->dn_numkeys);
	db_printf("Buf %p\n", node->fn_buf);
	if (NODE_FLAGS(node) & BT_INTERNAL) {
		bnode_ptr *p = fnode_getval(node, 0);
		db_printf("| C %lu |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = fnode_getval(node, i + 1);
			size_t __unused *t = fnode_getkey(node, i);
			db_printf("| K %lu || C %lu |", *t, *p);
		}
	} else {
		for (i = 0; i < NODE_SIZE(node); i++) {
			uint64_t __unused *t = fnode_getkey(node, i);
			diskptr_t __unused *v = fnode_getval(node, i);
			db_printf("| %lu -> %lu, %lu |,", *t, v->offset, v->size);
		}
	}
	db_printf("\n");
}

#endif /* DDB */
