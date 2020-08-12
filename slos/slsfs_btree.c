
#include <sys/param.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/bufobj.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/pctrie.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>

#include "slosmm.h"
#include "slos.h"
#include "slsfs.h"
#include "btree.h"
#include "slos_inode.h"
#include "slsfs_alloc.h"
#include "slsfs_buf.h"

#define NODE_LOCK(node, flags) (BUF_LOCK((node)->fn_buf, flags, NULL))
#define NODE_ISLOCKED(node) (BUF_ISLOCKED((node)->fn_buf))
#define NODE_ALLOC(flags) ((struct fnode *)uma_zalloc(fnode_zone, flags))
#define NODE_FREE(node) (((struct fnode *)(node))->fn_status |= FN_DEAD)
#define BP_ISCOWED(bp) (((bp)->b_fsprivate3) != 0)
#define BP_UNCOWED(bp) (((bp)->b_fsprivate3) = 0)
#define BP_SETCOWED(bp) (((bp)->b_fsprivate3) = (void *)1)

#define INDEX_INVAL (-1)

#define BP_TOFNODE(node) ((struct fnode *)(node)->b_fsprivate2)

static MALLOC_DEFINE(M_SLOS_BTREE, "slos btree", "SLOS Btrees");
uma_zone_t fnode_zone;

static void
fbtree_execrc(struct fbtree *tree)
{
	struct fbtree_rcentry *entry;
	SLIST_FOREACH(entry, &tree->bt_rcfn, rc_entry) {
		entry->rc_fn(entry->rc_ctx, tree->bt_root);
	}
}

/*
 * Registers a callback for when the fbtree changes its root
 */
void
fbtree_reg_rootchange(struct fbtree *tree, rootchange_t fn, void *ctx)
{
	struct fbtree_rcentry *entry = malloc(sizeof(struct fbtree_rcentry), M_SLOS_BTREE, M_WAITOK);
	entry->rc_fn = fn;
	entry->rc_ctx = ctx;
	SLIST_INSERT_HEAD(&tree->bt_rcfn, entry, rc_entry);
}

/* Requires to be void *, to deal with zone contructor and deconstructor 
 * typedefs 
 */
int 
fnode_construct(void *mem, int size, void *arg, int flags)
{
	struct fnode *node = (struct fnode *)mem;
	bzero(node, sizeof(struct fnode));

	return (0);
}

void 
fnode_deconstruct(void *mem, int size, void *arg)
{
}

static void
fbtree_stats(struct fbtree *tree)
{
	DEBUG("Type : Count");
	DEBUG1("Inserts : %lu", tree->bt_inserts);
	DEBUG1("Removes : %lu", tree->bt_removes);
	DEBUG1("Replaces : %lu", tree->bt_replaces);
	DEBUG1("Gets : %lu", tree->bt_gets);
	DEBUG1("Splits : %lu", tree->bt_splits);
	DEBUG1("Root replaces : %lu", tree->bt_root_replaces);
}

/*
 * Getters and setters for the keys and values of the btree. The type argument
 * is a key as to whether the node is internal or external.
 */
void *
fnode_getkey(struct fnode *node, int i)
{
	KASSERT(i <= NODE_MAX(node), ("Not getting key past max - %p - %d - %d", node, i, NODE_MAX(node)));
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
fnode_setkey(struct fnode *node, int i, void *val)
{
	KASSERT(i < NODE_MAX(node), ("Not inserting key past max"));
	void *at = fnode_getkey(node, i);
	memcpy((char *)at, val, node->fn_tree->bt_keysize);
}

static void
fnode_setval(struct fnode *node, int i, void *val)
{
	KASSERT((((NODE_TYPE(node) == BT_INTERNAL) || (NODE_TYPE(node) == BT_BUCKET))  && i <= NODE_MAX(node)) ||
		i < NODE_MAX(node), ("Not inserting val past max - %p  - %d", node, i));
	if (val == NULL) {
		bzero(fnode_getval(node, i), NODE_VS(node));
	} else {
		memcpy(fnode_getval(node, i), val, NODE_VS(node));
	}
}

void
fnode_print(struct fnode *node)
{
#ifdef KTR 
	int i;
	if (node == NULL) {
		return;
	}

	DEBUG3("%s - NODE(%p): %lu\n", node->fn_tree->bt_name, node, node->fn_location);
	DEBUG2("(%u)size (%u)type\n", NODE_SIZE(node), NODE_TYPE(node));
	if (NODE_TYPE(node) == BT_INTERNAL) {
		bnode_ptr *p = fnode_getval(node, 0);
		DEBUG1("| C %lu |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = fnode_getval(node, i + 1);
			uint64_t __unused *t = fnode_getkey(node, i);
			DEBUG2("| K %lu|| C %lu |", *t, *p);
		}
		DEBUG("Child memory\n");
		for (int i = 0; i <= NODE_SIZE(node); i++) {
			if (node->fn_pointers[i] != NULL) {
				struct fnode *n = (struct fnode *)node->fn_pointers[i];
				DEBUG1("| node(%lu) |", n->fn_location);
			} else {
				DEBUG("| NULL |");
			}
		}
	} else if (NODE_TYPE(node) == BT_EXTERNAL) {
		for (i = 0; i < NODE_SIZE(node); i++) {
			uint64_t *t = fnode_getkey(node, i);
			if (node->fn_tree->bt_valsize == sizeof(diskptr_t)) {
				diskptr_t *v = fnode_getval(node, i);
				DEBUG5("| %lu (%u)-> %lu, %lu, %lu |,", *t, node->fn_types[i], v->offset, v->size, v->epoch);
			} else {
				uint64_t *v = fnode_getval(node, i);
				DEBUG3("| %lu (%u)-> %lu|,", *t, node->fn_types[i], *v);
			}
		}
	}
#endif /* KTR */
}


/*
 * Print an external node and its left neighbor, if it exists.
 */
void
fnode_print_left_neighbor(struct fnode *fnode)
{
	struct fnode *left, *parent;
	bnode_ptr ptr;
	int i;

	/*
	 * Go to the parent of the current node. If we have no parents, we have
	 * no neighbors, so just print ourselves.
	 */
	fnode_parent(fnode, &parent);
	if (parent == NULL) {
		fnode_print(fnode);
		return ;
	}

	/* Get the offset in the parent. */
	for (i = 0; i < NODE_SIZE(parent) + 1; i++) {
		if (fnode == parent->fn_pointers[i])
			break;
	}
	KASSERT(i <= NODE_SIZE(parent),
	    ("Did not find fnode %p in parent %p", fnode, parent));

	/* Print the parent of the node. */
	fnode_print(parent);

	/* If we have an intermediate left neighbor, print it. */
	if (i > 0) {
		ptr = * (bnode_ptr *) fnode_getval(parent, i - 1);
		fnode_init(fnode->fn_tree, ptr, (struct fnode **) &parent->fn_pointers[i - 1]);
		left = parent->fn_pointers[i - 1];
		fnode_print(left);
	}

	/* Print the node itself. */
	fnode_print(fnode);
}

/*
 * Print an external node and its left and right neighbors, if they exist.
 */
void
fnode_print_neighbors(struct fnode *node)
{
	struct fnode *left, *right, *parent;
	bnode_ptr ptr;
	int i;

	/*
	 * Go to the parent of the current node. If we have no parents, we have
	 * no neighbors, so just print ourselves.
	 */
	fnode_parent(node, &parent);
	if (parent == NULL) {
		fnode_print(node);
		return ;
	}

	/* Get the offset in the parent. */
	for (i = 0; i < NODE_SIZE(parent) + 1; i++) {
		if (node == parent->fn_pointers[i])
			break;
	}
	KASSERT(i <= NODE_SIZE(parent),
	    ("Did not find fnode %p in parent %p", node, parent));

	/* Print the parent of the node. */
	fnode_print(parent);

	/* If we have an intermediate left neighbor, print it. */
	if (i > 0) {
		ptr = * (bnode_ptr *) fnode_getval(parent, i - 1);
		fnode_init(node->fn_tree, ptr, (struct fnode **) &parent->fn_pointers[i - 1]);
		left = parent->fn_pointers[i - 1];
		fnode_print(left);
	}

	/* Print the node itself. */
	fnode_print(node);

	/* Same for a right neighbor. */
	if (i < NODE_SIZE(parent)) {
		ptr = * (bnode_ptr *) fnode_getval(parent, i + 1);
		fnode_init(node->fn_tree, ptr, (struct fnode **) &parent->fn_pointers[i + 1]);
		right = parent->fn_pointers[i + 1];
		fnode_print(right);
	}
}


/* Print a whole level of the btree, starting from a subtree root. */
void
fnode_print_level(struct fnode *node)
{
	struct fnode *right;

	KASSERT(node != NULL, ("nonexistent node"));
	do {
		fnode_print(node);
		fnode_right(node, &right);
		node = right;
	} while (node != NULL);
}

/* Print an internal node and all its immediate children. */
void
fnode_print_internal(struct fnode *parent)
{
	struct fnode *child;
	int error;
	int i;

	KASSERT((NODE_TYPE(parent) == BT_INTERNAL) != 0, ("Checking external node"));

	DEBUG("=====PARENT=====");
	fnode_print(parent);
	for (i = 0; i <= NODE_SIZE(parent); i++) {
		/* Get the next child. If */
		error = fnode_fetch(parent, i, &child);
		if (error != 0) {
			DEBUG2("Warning: fnode_fetch failed with %d in %s", error, __func__);
			return;
		}
		DEBUG1("-----CHILD %d-----", i);
		fnode_print(child);
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
fnode_getbufptr(struct fbtree *tree, bnode_ptr ptr, struct buf **bp)
{
	struct buf *buf;
	int error;

	struct slos *slos = ((struct slos_node*)tree->bt_backend->v_data)->sn_slos;

	KASSERT(ptr != 0, ("Should never be 0"));
	error = bread(tree->bt_backend, ptr, BLKSIZE(slos), curthread->td_ucred, &buf);
	if (error) {
		DEBUG("Error reading block");
		return (EIO);
	}

	buf->b_fsprivate2 = NULL;
	buf->b_fsprivate3 = 0;

	*bp = buf;

	return (0);
}

int
fnode_fetch(struct fnode *node, int index, struct fnode **next)
{
	bnode_ptr *ptr;
	struct fnode *tmpnode;
	int error = 0;
	MPASS(NODE_TYPE(node) == BT_INTERNAL);
#ifdef INVARIANTS
	if (NODE_TYPE(node) != BT_BUCKET) {
		for (int i = 0; i < NODE_SIZE(node) + 1; i++) {
			ptr = (bnode_ptr *)fnode_getval(node, i);
			if (node->fn_pointers[i] != NULL) {
				if (((struct fnode *)node->fn_pointers[i])->fn_location != *ptr) {
				    fnode_print(node);
				    panic("Problem with children %lu %d", *ptr, i);
				}
			}
		}
	}
#endif
	ptr = (bnode_ptr *)fnode_getval(node, index);
	if (*ptr == 0) {
		fnode_print(node);
		panic("Should not be zero %d", index);
	}
	error = fnode_init(node->fn_tree, *ptr, (struct fnode **)&node->fn_pointers[index]);
	tmpnode = (struct fnode *)node->fn_pointers[index];
	KASSERT(tmpnode->fn_location == *ptr, ("Should be equal new"));

	// We have to do this cause the buffer may have been freed from under 
	// us so we init to check initilize in case this has occured
	*next = tmpnode;
	return (error);
}

/*
 * Find the index of the first key >= the target (or NODE_SIZE(node) on failure).
 */
static int
fnode_first_greater_equal(struct fnode *node, const void *target)
{
	int start = 0;
	int end = NODE_SIZE(node);
	int mid;
	const void *key;

	while (start < end) {
		mid = start + (end - start) / 2;
		key = fnode_getkey(node, mid);
		if (NODE_COMPARE(node, key, target) < 0) {
			start = mid + 1;
		} else {
			end = mid;
		}
	}

	return (start);
}

/*
 * Find the index of the first key >= the target (or NODE_SIZE(node) on failure).
 */
static int
fnode_first_greater(struct fnode *node, const void *target)
{
	int start = 0;
	int end = NODE_SIZE(node);
	int mid;
	const void *key;

	while (start < end) {
		mid = start + (end - start) / 2;
		key = fnode_getkey(node, mid);
		if (NODE_COMPARE(node, key, target) <= 0) {
			start = mid + 1;
		} else {
			end = mid;
		}
	}

	return (start);
}

/*
 * Follows the key through the btree, using a modified binary search (first key
 * greater than) to find the correct path. There is a modified use case where
 * the caller can specify the "to" argument which allows us to find the parent
 * of a node.
 *
 * root: Starting node to start traversing the tree
 * key: key to use to traverse the tree
 * to: node to stop at when traversing the tree
 * node: When to is not null, node will be set with the parent of the
 * "to" node.
 */
static int
fnode_follow(struct fnode *root, const void *key, struct fnode *to, struct fnode **node)
{
	int error = 0;
	struct fnode *next;
	struct fnode *cur;
	int index;

	cur = root;
	MPASS(root != NULL);

	while (NODE_TYPE(cur) == BT_INTERNAL) {
		index = fnode_first_greater(cur, key);

		if (to != NULL && to->fn_location == *(bnode_ptr *)fnode_getval(cur, index)) {
			*node = cur;
			return (0);
		} else {
			error = fnode_fetch(cur, index, &next);
			if (error != 0) {
				panic("Issue fetching");
			}
			MPASS(next != NULL);
		}
		/* Prepare to traverse the next node. */
		cur = next;
	}

	*node = cur;
	KASSERT(NODE_TYPE(*node) != BT_INTERNAL, ("Node still internal"));

	return (error);
}

/*
 * Function recursively starts at some buf, and moves up to its parent, and 
 * stop onces finding a ancestor that has already been cowed.
 *
 * Note: Callers should call fbtree_execrc afterwords, as any cow to any node 
 * will cause a root change.
 */
static int
fnode_cow(struct buf *bp)
{
	int error;
	diskptr_t ptr;
	bnode_ptr old;
	struct fnode *cur = BP_TOFNODE(bp);
	struct fbtree *tree = cur->fn_tree;
	struct fnode *parent;
	struct slos *slos = ((struct slos_node *)tree->bt_backend->v_data)->sn_slos;
	struct bufobj *bo = &tree->bt_backend->v_bufobj;

	ASSERT_BO_WLOCKED(bo);

	BUF_LOCK(cur->fn_buf, LK_EXCLUSIVE, 0);
	// Already cowed, we can stop
	if (BP_ISCOWED(cur->fn_buf)) {
		BUF_UNLOCK(cur->fn_buf);
		return (0);
	}

	BP_SETCOWED(cur->fn_buf);

	DEBUG3("fnode_cow(%p) %lu->%lu", bp, bp->b_lblkno, ptr.offset);
	// Alocate a new block the btree node
	error = ALLOCATEPTR(slos, BLKSIZE(slos), &ptr);
	MPASS(error == 0);

	BO_UNLOCK(bo);
	// Get the parent
	fnode_parent(cur, &parent);

	// Release the buf from the current vnode, this allows us to remove the 
	// logical mapping in the bufobject
	brelvp(cur->fn_buf);
	BO_LOCK(bo);

	/* Change the location of the block and place the buffer back on the 
	 *vnode backend. This allows us to essentially copy on write without 
	 *actually doing any copying.
	 */
	old = cur->fn_location;
	cur->fn_buf->b_lblkno = ptr.offset;
	cur->fn_location = ptr.offset;
	bgetvp(tree->bt_backend, cur->fn_buf);

	BO_UNLOCK(bo);
	// Final reassignment of buffer to proper blkno
	reassignbuf(cur->fn_buf);
	BO_LOCK(bo);

	if (parent) {
		int i;
		// Find exactly where you exist in your parents children values
		for (i = 0; i < NODE_SIZE(parent) + 1; i++) {
			bnode_ptr p = *(bnode_ptr *)fnode_getval(parent, i);
			if (old == p) {
				if (parent->fn_pointers[i] == NULL) {
					parent->fn_pointers[i] = cur;
				} 

				if (parent->fn_pointers[i] != cur) {
					fnode_print(parent->fn_pointers[i]);
					fnode_print(cur);
					panic("Problem with fixup");
				}
				break;
			}
		}

		if (i > NODE_SIZE(parent)) {
			fnode_print(parent);
			fnode_print(cur);
			panic("Problem with children in cow %lu %d", old, i);
		}
		// Replace the on disk ptr version of the current node
		fnode_setval(parent, i, &cur->fn_location);
		// Now do the same for the parent
		fnode_cow(parent->fn_buf);
	} 

	bdwrite(cur->fn_buf);

	return 0;
}



/*
 * Initialize an in-memory btree.
 */
int
fbtree_init(struct vnode *backend, bnode_ptr ptr, fb_keysize keysize,
    fb_valsize valsize, compare_t comp, char * name, uint64_t flags, struct fbtree *tree)
{
	struct fbtree_rcentry *entry;

	tree->bt_backend = backend;
	tree->bt_keysize = keysize;
	tree->bt_valsize = valsize;
	tree->comp = comp;
	tree->bt_root = ptr;
	tree->bt_flags = flags;
	strcpy(tree->bt_name, name);
	tree->bt_rootnode = NULL;

	if (!lock_initialized(&tree->bt_lock.lock_object)) {
		lockinit(&tree->bt_lock, PVFS, "Btree Lock", 0, LK_CANRECURSE);
	}

	while(!SLIST_EMPTY(&tree->bt_rcfn)) {
		entry = SLIST_FIRST(&tree->bt_rcfn);
		SLIST_REMOVE_HEAD(&tree->bt_rcfn, rc_entry);
		free(entry, M_SLOS_BTREE);
	}

	return (0);
}

/*
 * Fnode right retrieves the next btree node to it.
 * If required the node will traverse upwards and call right on the parent to 
 * retrieve its sibling.  This is due to the complexity of updating pointers 
 * when dealing with copy on write.
 */
int
fnode_right(struct fnode *node, struct fnode **right)
{
	int error;
	int index;
	struct fnode *parent;

	for (;;) {
		error = fnode_parent(node, &parent);
		if (error != 0) {
			*right = NULL;
			return (error);
		}

		if (parent == NULL) {
			*right = NULL;
			return (0);
		}

		// Find our index in the parent
		for (index = 0; index <= NODE_SIZE(parent); ++index) {
			if (parent->fn_pointers[index] == node) {
				break;
			}
		}
		KASSERT(index <= NODE_SIZE(parent), ("Node btree node %p not in parent %p", node, parent));

		// If we're the rightmost child, keep going up, else break
		if (index < NODE_SIZE(parent)) {
			++index;
			break;
		}

		node = parent;
	}

	// Go back down the tree, as far right as possible
	while (NODE_TYPE(parent) == BT_INTERNAL) {
		error = fnode_fetch(parent, index, &node);
		if (error != 0) {
			*right = NULL;
			return (error);
		}

		index = 0;
		parent = node;
	}

	*right = node;
	return (0);
}

int
fbtree_sync(struct fbtree *tree)
{
	struct buf *bp, *tbd;
	struct bufobj *bo = &tree->bt_backend->v_bufobj;
	struct fnode *node;
	int attempts = 0;
	BTREE_LOCK(tree, LK_EXCLUSIVE);
	VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);

	BO_LOCK(bo);
tryagain:
	if (bo->bo_dirty.bv_cnt) {
		TAILQ_FOREACH_SAFE(bp, &bo->bo_dirty.bv_hd, b_bobufs, tbd) {
			fnode_cow(bp);
		}

		tree->bt_root = tree->bt_rootnode->fn_location;

		TAILQ_FOREACH_SAFE(bp, &bo->bo_dirty.bv_hd, b_bobufs, tbd) {
			BUF_LOCK(bp, LK_EXCLUSIVE, NULL);
			node = BP_TOFNODE(bp);
			/*
			 * This seems to occur due to buffer reassignment, 
			 * placing this as a TODO to see if this is actually 
			 * necessary but currently it works.
			 */
			if (!BP_ISCOWED(bp)) {
				BUF_UNLOCK(bp);
				attempts++;
				if (attempts > 100) {
					panic("Reassigning of bufs causing issues");
				}
				goto tryagain;
			}
			BUF_UNLOCK(bp);
		}

		TAILQ_FOREACH_SAFE(bp, &bo->bo_dirty.bv_hd, b_bobufs, tbd) {
			BUF_LOCK(bp, LK_EXCLUSIVE | LK_INTERLOCK, BO_LOCKPTR(bo));
			node = BP_TOFNODE(bp);
			if (!BP_ISCOWED(bp)) {
				panic("Reassigning of bufs causing issues");
			}
			MPASS(node->fn_buf == bp);
			MPASS((node->fn_status & FN_DEAD) == 0);
			bp->b_fsprivate2 = NULL;
			DEBUG4("Freeing node %p, %p, %p, %p", bp, tree->bt_backend, node, tree->bt_backend->v_data);
			NODE_FREE(node);
			bp->b_flags &= ~B_MANAGED;
			bwrite(bp);
			BP_UNCOWED(bp);
			BO_LOCK(bo);
		}
		MPASS(bo->bo_dirty.bv_cnt == 0);
		BO_UNLOCK(bo);

		tree->bt_rootnode = NULL;
		fbtree_execrc(tree);
	} else {
		BO_UNLOCK(bo);
	}

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);
	return (0);
}


int
fbtree_sync_withalloc(struct fbtree *tree, diskptr_t *ptr)
{

	struct buf *bp, *tbd;
	struct bufobj *bo = &tree->bt_backend->v_bufobj;
	VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);
	BO_LOCK(bo);
	TAILQ_FOREACH_SAFE(bp, &bo->bo_dirty.bv_hd, b_bobufs, tbd) {
		BUF_LOCK(bp, LK_EXCLUSIVE | LK_INTERLOCK, BO_LOCKPTR(bo));
		slsfs_bundirty(bp);
		BO_LOCK(bo);
	}
	BO_UNLOCK(bo);

	VOP_UNLOCK(tree->bt_backend, 0);
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
	error = fnode_follow(tree->bt_rootnode, &key, NULL, &node);
	if (error) {
		DEBUG("Problem following node");
		return (error);
	}

	/* Move to the right, counting how many keys we come across. */
	struct fnode *right = NULL;
	fnode_right(node, &right);
	while(right != NULL) {
		size += node->fn_dnode->dn_numkeys;
		node = right;
		fnode_right(node, &right);
	};

	size += node->fn_dnode->dn_numkeys;

	return size;
}

#ifdef INVARIANTS
static void
fnode_keymin_check(const void *key, const struct fnode_iter *iter)
{
	const void *keyt;
	struct fnode *node = iter->it_node, *parent;
	struct fnode_iter next;

	if (!ITER_ISNULL(*iter)) {
		// Check that the returned node is <= the key
		keyt = ITER_KEY(*iter);
		KASSERT(NODE_COMPARE(node, keyt, key) <= 0, ("keymin too big"));

		// Check that the next node is > the key
		next = *iter;
		fnode_iter_next(&next, 0);
		if (!ITER_ISNULL(next)) {
			keyt = ITER_KEY(next);
			if (NODE_COMPARE(node, keyt, key) <= 0) {
				printf("%lu %lu", *(const uint64_t*)keyt, *(const uint64_t*)key);
				printf("1st parent");
				fnode_parent(iter->it_node, &parent);
				fnode_print(parent);
				fnode_print_level(iter->it_node);
				panic("keyt %lu %lu", ITER_KEY_T(next, uint64_t), ITER_KEY_T(*iter, uint64_t));
			}
		}
	}
}
#endif

/*
 * Find the location of the largest key smaller than or equal to the one provided.
 */
static int
fnode_keymin_iter(struct fnode *root, void *key, struct fnode_iter *iter)
{
	int error;

	// Follow catches if this is an external node or not
	error = fnode_follow(root, key, NULL, &iter->it_node);
	if (error) {
		return (error);
	}

	KASSERT(NODE_TYPE(iter->it_node) != (BT_INTERNAL), ("Should be on a external node now"));

	// Binary search for the first element > key
	iter->it_index = fnode_first_greater(iter->it_node, key);

	// Go back to find the last element <= key
	fnode_iter_prev(iter);

#ifdef INVARIANTS
	fnode_keymin_check(key, iter);
#endif

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

	if (ITER_ISNULL(iter)) {
		return EINVAL;
	}

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
		DEBUG("Problem initing");
		return (error);
	}

	error = fnode_keymin_iter(tree->bt_rootnode, key, iter);
	return (error);
}

/*
 * If at the end of a node, skip to the next node.
 */
static void
fnode_iter_skip(struct fnode_iter *it)
{
	//KASSERT(it->it_index <= NODE_SIZE(it->it_node), ("Iterator was out of 
	//bounds"));

	/* If we've exhausted the current node, skip. */
	struct fnode *right;
	if (it->it_index >= NODE_SIZE(it->it_node)) {
		fnode_right(it->it_node, &right);
		if (right == NULL) {
			/* End of the tree. */
			it->it_index = INDEX_INVAL;
		} else {
			/* Otherwise switch nodes. */
			MPASS(NODE_SIZE(right) > 0);
			it->it_node = right;
			it->it_index = 0;
		}
	}
}

#ifdef INVARIANTS
static void
fnode_keymax_check(const void *key, const struct fnode_iter *iter)
{
	const void *keyt;

	if (!ITER_ISNULL(*iter)) {
		// We should be in bounds at this point.
		KASSERT(iter->it_index < NODE_SIZE(iter->it_node),
		    ("invalid index %d", iter->it_index));

		// Make sure the node actually has keys larger than key.
		keyt = fnode_getkey(iter->it_node, iter->it_index);
		KASSERT(NODE_COMPARE(iter->it_node, keyt, key) >= 0,
		    ("leftmost key %lu smaller than key %lu",
		    *(const uint64_t *)keyt, *(const uint64_t *)key));

		// Make sure this is the first one.
		if (iter->it_index > 0) {
			keyt = fnode_getkey(iter->it_node, iter->it_index - 1);
			KASSERT(NODE_COMPARE(iter->it_node, keyt, key) < 0, ("keymax too big"));
		}
	}
}
#endif

/*
 * Find the smallest key in the subtree that's larger than or equal to the key provided.
 */
static int
fnode_keymax_iter(struct fnode *root, const void *key, struct fnode_iter *iter)
{
	int error;

	// Follow catches if this is an external node or not
	error = fnode_follow(root, key, NULL, &iter->it_node);
	if (error) {
		return (error);
	}

	KASSERT(NODE_TYPE(iter->it_node) != (BT_INTERNAL), ("Should be on a external node now"));

	// Binary search for the first element >= key
	iter->it_index = fnode_first_greater_equal(iter->it_node, key);

	/*
	 * Due to the way we follow the value down the tree, we need to make
	 * sure we are actually the largest value in the tree, and there is no
	 * key to the right.
	 */
	fnode_iter_skip(iter);

#ifdef INVARIANTS
	fnode_keymax_check(key, iter);
#endif

	return (0);

}

/*
 * Find the smallest key in the subtree that's larger than or equal to the key.
 */
int
fnode_keymax(struct fnode *root, void *key, void *value)
{
	int error;
	struct fnode_iter iter;

	/* Get the location of the minimum key-value pair. */
	error = fnode_keymax_iter(root, key, &iter);
	if (error) {
		return (error);
	}

	if (ITER_ISNULL(iter)) {
		return EINVAL;
	}

	/* Copy the pair out and release the node buffer. */
	memcpy(value, ITER_VAL(iter), root->fn_tree->bt_valsize);
	memcpy(key, ITER_KEY(iter), root->fn_tree->bt_keysize);

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
 * All Btrees are backed by their own fake device vnode so we need to purge the
 * vnodes clean and dirty lists and then also free the fnodes attached to them
 */
void 
fbtree_destroy(struct fbtree *tree)
{
	struct buf *bp, *nbp;
	struct fbtree_rcentry *entry;
	struct bufobj *bo = &tree->bt_backend->v_bufobj;
	TAILQ_FOREACH_SAFE(bp, &bo->bo_clean.bv_hd, b_bobufs, nbp) {
		BUF_LOCK(bp, LK_EXCLUSIVE, 0);
		if (bp->b_flags & B_MANAGED) {
			bp->b_flags &= ~(B_MANAGED);
		} else {
			bremfree(bp);
		}
		if (bp->b_fsprivate2) {
			NODE_FREE(bp->b_fsprivate2);
			bp->b_fsprivate2 = NULL;
		}
		brelse(bp);
	}

	TAILQ_FOREACH_SAFE(bp, &bo->bo_dirty.bv_hd, b_bobufs, nbp) {
		BUF_LOCK(bp, LK_EXCLUSIVE, 0);
		if (bp->b_flags & B_MANAGED) {
			bp->b_flags &= ~(B_MANAGED);
		} else {
			bremfree(bp);
		}

		if (bp->b_fsprivate2) {
			NODE_FREE(bp->b_fsprivate2);
			bp->b_fsprivate2 = NULL;
		}
		brelse(bp);
	}

	while(!SLIST_EMPTY(&tree->bt_rcfn)) {
		entry = SLIST_FIRST(&tree->bt_rcfn);
		SLIST_REMOVE_HEAD(&tree->bt_rcfn, rc_entry);
		free(entry, M_SLOS_BTREE);
	}

	vinvalbuf(tree->bt_backend, 0, 0, 0);
	vnode_destroy_vobject(tree->bt_backend);
}

/*
 *  Iterate through the keys of the btree, traversing external bnodes if necessary.
 */
int
fnode_iter_next(struct fnode_iter *it, int bucket_traverse)
{
	struct dnode __unused *dn = it->it_node->fn_dnode;
	struct fnode *bucket;

	/* An invalid iterator is still invalid after iteration. */
	if (it->it_index == INDEX_INVAL) {
		return (0);
	}

	KASSERT(dn->dn_numkeys > 0, ("Started iterating from an empty node"));
	KASSERT(it->it_index < dn->dn_numkeys, ("Iterator was out of bounds %u %u", it->it_index, dn->dn_numkeys));

	if (NODE_TYPE(it->it_node) == BT_BUCKET) {
		if (it->it_index == (NODE_SIZE(it->it_node) - 1)) {
			if (BUCKET_HASNEXT(it->it_node)) {
				BUCKET_GETNEXT(it->it_node, &it->it_node);
				it->it_index = 0;
			} else {
				fbtree_keymax_iter(it->it_node->fn_tree, fnode_getkey(it->it_node, 0), it);
				fnode_iter_next(it, 0);
			}
		} else {
			++it->it_index;
		}
	} else {
		if (ITER_ISBUCKETAT(*it) && bucket_traverse && 
			(it->it_node->fn_tree->bt_flags & FN_ALLOWDUPLICATE)) {
			fnode_fetch(it->it_node, it->it_index, &bucket);
			it->it_node = bucket;
			it->it_index = 0;

		} else  {
			++it->it_index;
			fnode_iter_skip(it);
		}
	}

	return (0);
};

static int
fnode_left(struct fnode *node, struct fnode **left)
{
	int error;
	int index;
	struct fnode *parent;

	for (;;) {
		error = fnode_parent(node, &parent);
		if (error != 0) {
			*left = NULL;
			return (error);
		}

		if (parent == NULL) {
			*left = NULL;
			return (0);
		}

		// Find our index in the parent
		for (index = 0; index <= NODE_SIZE(parent); ++index) {
			if (parent->fn_pointers[index] == node) {
				break;
			}
		}
		KASSERT(index <= NODE_SIZE(parent), ("Node btree node %p not in parent %p", node, parent));

		// If we're the leftmost child, keep going up, else break
		if (index > 0) {
			--index;
			break;
		}

		node = parent;
	}

	// Go back down the tree, as far right as possible
	while (NODE_TYPE(parent) == BT_INTERNAL) {
		error = fnode_fetch(parent, index, &node);
		if (error != 0) {
			*left = NULL;
			return (error);
		}

		index = NODE_SIZE(node);
		parent = node;
	}

	*left = node;
	return (0);
}

int
fnode_iter_prev(struct fnode_iter *iter)
{
	struct fnode *left;

	/* An invalid iterator is still invalid after iteration. */
	if (iter->it_index == INDEX_INVAL) {
		return (0);
	}

	if (iter->it_index == 0) {
		fnode_left(iter->it_node, &left);
		if (left != NULL) {
			iter->it_node = left;
			iter->it_index = NODE_SIZE(left);
		}
	}

	--iter->it_index;
	return (0);
}

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

void
fnode_setup(struct fnode *node, struct fbtree *tree, bnode_ptr ptr)
{
	node->fn_dnode = (struct dnode *)node->fn_buf->b_data;
	node->fn_location = ptr;
	node->fn_tree = tree;
	if (NODE_TYPE(node) == BT_INTERNAL) {
		node->fn_types = NULL;
		node->fn_keys = (char *)node->fn_dnode->dn_data;
		node->fn_values = (char *)node->fn_keys + (NODE_MAX(node) * NODE_KS(node));
		fnode_getval(node, NODE_MAX(node) - 1);
	} else if (NODE_TYPE(node) == BT_BUCKET){
		node->fn_types = NULL;
		node->fn_keys = (char *)node->fn_dnode->dn_data;
		node->fn_values = (char *)node->fn_keys + (NODE_KS(node));
		fnode_getval(node, NODE_MAX(node) - 1);
	} else {
		node->fn_types = (uint8_t *)node->fn_dnode->dn_data;
		node->fn_keys = (char *)node->fn_types + (sizeof(uint8_t) * NODE_MAX(node));
		node->fn_values = (char *)node->fn_keys + (NODE_MAX(node) * NODE_KS(node));
		fnode_getval(node, NODE_MAX(node) - 1);
	}

	node->fn_buf->b_fsprivate2 = node;
	node->fn_status = 0;
}

/*
 * Create a node for the btree at the specified offset.
 */
static int
fnode_create(struct fbtree *tree, bnode_ptr ptr, struct fnode *node, uint8_t type)
{
	struct slos *slos =((struct slos_node*)tree->bt_backend->v_data)->sn_slos;
	KASSERT(ptr != 0, ("Why are you making a blk on the allocator block"));

	/* Get the new bnode's block into the cache. */
	node->fn_buf = getblk(tree->bt_backend, ptr, BLKSIZE(slos), 0, 0, 0);
	if (node->fn_buf == NULL) {
		panic("Fnode create failed");
	}

	bzero(node->fn_buf->b_data, node->fn_buf->b_bcount);
	node->fn_dnode = (struct dnode *)node->fn_buf->b_data;
	node->fn_dnode->dn_flags = type;
	node->fn_dnode->dn_numkeys = 0;

	fnode_setup(node, tree, ptr);
	bzero(node->fn_pointers, sizeof(void *) * 300);

	// Node max is dependent on type to calculate correctly
	bqrelse(node->fn_buf);

	return (0);
}


/*
 * Allocate a new for the tree, along with any necessary on-disk resources.
 */
static int
fbtree_allocnode(struct fbtree *tree, struct fnode **created, uint8_t type)
{
	diskptr_t ptr;
	int error = 0;

	struct slos *slos = ((struct slos_node*)(tree->bt_backend->v_data))->sn_slos;
	struct fnode *tmp = NODE_ALLOC(M_WAITOK);

	error = ALLOCATEPTR(slos, BLKSIZE(slos), &ptr);
	if (error) {
		return (error);
	}

	error = fnode_create(tree, ptr.offset, tmp, type);
	if (error != 0) {
		NODE_FREE(tmp);
		return (error);
	}

	*created = tmp;
	MPASS(NODE_TYPE(*created) == type);

	return (0);
}

/*
 * Insert a key-value pair into a node at the specified offset.
 */
static void
fnode_insert_at(struct fnode *node, void *key, void *value, int i)
{
	char *src;

	if (i >= NODE_MAX(node)) {
		fnode_print(node);
	}
	KASSERT(i < NODE_MAX(node), ("Insert is out of bounds at %d", i));

	/* The keys scoot over to make room for the new one. */
	src = fnode_getkey(node, i);
	memmove(src + node->fn_tree->bt_keysize, src, (NODE_SIZE(node) - i) * NODE_KS(node));
	fnode_setkey(node, i, key);
	node->fn_dnode->dn_numkeys += 1;

	/* The offset of the value in the node depends if it's internal or 
	 * external. */
	if (NODE_TYPE(node) == BT_INTERNAL) {
		MPASS(node->fn_types == NULL);
		src = fnode_getval(node, i + 1);
		/* Move over the on-disk children, and add the new one.  */
		memmove(src + NODE_VS(node), src, (NODE_SIZE(node) - i) * NODE_VS(node));
		fnode_setval(node, i + 1, value);
		/* Same for in-memory, mark the new one as nonresident.  */
		memmove(&node->fn_pointers[i + 2], &node->fn_pointers[i + 1], (NODE_SIZE(node) - i) * sizeof(void *));
		node->fn_pointers[i + 1] = NULL;
	} else {
		MPASS(NODE_TYPE(node) == BT_EXTERNAL);
		/* Otherwise just scoot over the values, no pointers here.  */
		src = fnode_getval(node, i);
		memmove(src + NODE_VS(node), src, (NODE_SIZE(node) - i) * NODE_VS(node));
		memmove(&node->fn_types[i + 1], &node->fn_types[i], (NODE_SIZE(node) - i) * sizeof(uint8_t));
		node->fn_types[i] = 0;
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
	
	error = fbtree_allocnode(node->fn_tree, &newroot, BT_INTERNAL);
	if (error) {
		panic("Problem creating root");
		return (error);
	}

	newroot->fn_dnode->dn_numkeys = 0;
	newroot->fn_dnode->dn_flags = BT_INTERNAL;

	/*
	 * The new root's child is the old root. This makes it imbalanced,
	 * but the caller should fix it up.
	 */
	fnode_setval(newroot, 0, &node->fn_location);
	newroot->fn_pointers[0] = node;

	// XXX Gotta inform the inode its attached to to change? Do we pass it a
	// function when this occurs to execute the update on it?

	KASSERT(node->fn_location != newroot->fn_location, ("Should be new!"));

	/* Update the tree. */
	tree->bt_root = newroot->fn_location;
	tree->bt_rootnode = newroot;
	*root = newroot;
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
	NODE_TYPE(right) = NODE_TYPE(left);
	if (NODE_TYPE(left) == BT_INTERNAL) {
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
	if (NODE_TYPE(left) == BT_INTERNAL) {
		/* Skip key i, we'll put it in the parent node. */
		src = fnode_getkey(left, i + 1);
	} else {
		src = fnode_getkey(left, i);
	}

	/* Copy over keys array from the left node the right. */
	memcpy(fnode_getkey(right, 0), src, NODE_KS(right) * NODE_SIZE(right));

	/* Copy over the values. */
	if (NODE_TYPE(left) == BT_INTERNAL) {
		/* Value i stays with the left node, so start from value i + 1. */
		src = fnode_getval(left, i + 1);
		memcpy(fnode_getval(right, 0), src,  sizeof(bnode_ptr) * (NODE_SIZE(right) + 1));
		memcpy(right->fn_pointers, &left->fn_pointers[i + 1],  sizeof(void *) * (NODE_SIZE(right) + 1));
		bzero(&left->fn_pointers[i + 1], sizeof(void *) * (300 - i + 1));
	} else {
		src = fnode_getval(left, i);
		memcpy(fnode_getval(right, 0), src,  NODE_VS(right) * NODE_SIZE(right));
		memcpy(right->fn_types,  &left->fn_types[i], NODE_VS(right) * sizeof(uint8_t));
	}
}

int
fnode_parent(struct fnode *node, struct fnode **parent)
{
	int error;
	if (node == node->fn_tree->bt_rootnode) {
		*parent = NULL;
		return (0);
	}

	error = fnode_follow(node->fn_tree->bt_rootnode, fnode_getkey(node, 0), node, parent);
	MPASS(!error);

	return (error);
}

#ifdef INVARIANTS
static int 
fnode_verifypointers(struct fnode *node)
{
	if (NODE_TYPE(node) == BT_INTERNAL) {
		for (int i = 0; i <= NODE_SIZE(node); i++) {
			bnode_ptr p = *(bnode_ptr *)fnode_getval(node, i);
			if (node->fn_pointers[i] != NULL) {
				struct fnode *n = (struct fnode *)node->fn_pointers[i];
				KASSERT(p == n->fn_location, ("Should be the same"));
			}
		}
	}
	return (0);
}


static int
fnode_verifysplit(void *space, struct fnode *left, struct fnode *right, struct fnode *parent, void *op, size_t mid)
{
	struct fnode node, oldparent;
	struct fnode_iter first, split;
	bnode_ptr p;
	node.fn_dnode = space;
	node.fn_location = INDEX_INVAL;
	node.fn_keys = node.fn_dnode->dn_data;
	node.fn_tree = left->fn_tree;
	node.fn_values = (char *)node.fn_keys + (NODE_MAX(&node) * NODE_KS(&node));
	node.fn_status = 0;

	oldparent.fn_dnode = op;
	oldparent.fn_location = INDEX_INVAL;
	oldparent.fn_keys = oldparent.fn_dnode->dn_data;
	oldparent.fn_tree = left->fn_tree;
	oldparent.fn_values = (char *)oldparent.fn_keys + (NODE_MAX(&oldparent) * NODE_KS(&oldparent));
	oldparent.fn_status = 0;

	first.it_node = &node;
	first.it_index = 0;

	split.it_node = left;
	split.it_index = 0;

	if (NODE_TYPE(&node) == BT_INTERNAL) {
	} else {
		KASSERT(NODE_SIZE(&node) == NODE_SIZE(left) + NODE_SIZE(right), ("Should be equal"));
		while (first.it_index < NODE_SIZE(&node)) {
			KASSERT(!ITER_ISNULL(split), ("This should not be null"));
			uint64_t k1 = ITER_KEY_T(first, uint64_t);
			uint64_t k2 = ITER_KEY_T(split, uint64_t);
			KASSERT(k1 == k2, ("Should be equal %lu - %lu", k1, k2));
			first.it_index += 1;
			ITER_NEXT(split);
		}

		// Key structure is OK - moving to make sure parent is 
		// correctly done
		uint64_t key = *(uint64_t *)fnode_getkey(&node, mid);
		uint64_t fkey = *(uint64_t *)fnode_getkey(right, mid);
		KASSERT(key >= fkey, ("Key should follow invariant %lu : %lu", key, fkey));

		first.it_node = &oldparent;
		first.it_index = 0;
		// Assert parent is correct
		for (int i = 0; i < NODE_SIZE(parent); i++) {
			uint64_t k1 = *(uint64_t *)fnode_getkey(parent, i);
			// Found the key check is children are correctly placed
			if (NODE_COMPARE(parent, &k1, &key) == 0) {
				p = *(bnode_ptr *)fnode_getval(parent, i);
				KASSERT(p == right->fn_location, ("Should be left child"));
				i++;
			} else {
				uint64_t k2 = ITER_KEY_T(first, uint64_t);
				p = *(bnode_ptr *)fnode_getval(parent, i);
				bnode_ptr old = *(bnode_ptr *)fnode_getval(&oldparent, i);
				KASSERT(k1 == k2, ("Should still have same keys"));
				KASSERT(p == old, ("Children should still be the same"));
				ITER_NEXT(first);
			}
		}
	}
	free(space, M_SLOS_BTREE);
	free(op, M_SLOS_BTREE);
	return (0);
}
#endif // INVARIANTS

/*
 * Split a btree node in two children and reorganize the tree accordingly.
 */
static int
fnode_split(struct fnode *node)
{
	struct fnode *right;
	struct fnode *parent = NULL;
	int error;
	int newroot = 0;
	size_t mid_i;
	uint64_t newkey;

	// This signifies the root node, if this is the case we need handle
	// specifically as we need 2 node allocations specifically
	/* Make the left node. If we're splitting the root we need extra bookkeeping */
	if (node == node->fn_tree->bt_rootnode) {
		error = fnode_newroot(node, &parent);
		if (error) {
			DEBUG("ERROR CREATING NEW ROOT");
			return (error);
		}
		KASSERT(node->fn_location != node->fn_tree->bt_root, ("Problem with fnode - %p", node));
		newroot = 1;
	} else {
		fnode_parent(node, &parent);
		if (parent == NULL) {
			fnode_print(node);
			panic("No parent found");
		}
	}

	/* Make the right node. */
	error = fbtree_allocnode(node->fn_tree, &right, NODE_TYPE(node));
	if (error) {
		panic("Problem creating split pair");
		return (error);
	}

	KASSERT(right->fn_location != 0, ("Why did this get allocated here"));
	KASSERT(parent->fn_dnode->dn_flags & BT_INTERNAL, ("Should be an internal node"));

	/*
	 * Redistribute the original node's contents to the two new ones. We
	 * split the keys equally.
	 */
	mid_i = NODE_SIZE(node) / 2;
	newkey = *(uint64_t *)fnode_getkey(node, mid_i);
	fnode_balance(node, right, mid_i);
	// Don't need to write the left node, as once it pops from insert it
	// will be dirtied.
	/*
	 * Insert the new node to the parent. If the parent is
	 * at capacity, this will cause a split it.
	 *
	 * This is indirectly recursive, but btrees are shallow so there is no problem
	 */
	if (NODE_TYPE(right) != BT_INTERNAL) {
		newkey = *(uint64_t *)fnode_getkey(right, 0);
	}

	error = fnode_insert(parent, &newkey, &right->fn_location);
	if (error) {
		return (error);
	}
	// We have to keep track of it because it is dirty and the syncer will 
	// So call parent so allow for it the be preset in the tree of 
	// pointers.  We need to call parent as well cause the insert into its 
	// parent could cause node and right to have different parents now.
	fnode_parent(right, &parent);
	fnode_write(right);
	fnode_parent(node, &parent);
	fnode_write(node);

	uma_zfree(fnode_zone, right);
	if (newroot) {
		fbtree_execrc(parent->fn_tree);
	}

	DEBUG2("split %p, %p", node, right);
	return (0);
}

/*
 * Insert a key-value pair into an internal node.
 */
static int
fnode_internal_insert(struct fnode *node, void *key, bnode_ptr *val)
{
	int error = 0;
	int index;

	/* Find the proper position in the bnode to insert. */
	index = fnode_first_greater_equal(node, key);

	fnode_insert_at(node, key, val, index);

	/* Split the internal node if over capacity. */
	if ((NODE_SIZE(node) + 1) == NODE_MAX(node)) {
		error = fnode_split(node);
	}

	return (error);
}

/*
 * Buckets are just linked lists of pages, with the last "value"
 * being the next pointer, so buckets are NODE_MAX(node) - 1 in size
 */
static int
fnode_bucket_insert(struct fnode *parent, struct fnode *node, int at, void *key, void *value)
{
	int error;
	struct fnode *bucket = NULL;

	// Get next bucket if this is full
	if (NODE_SIZE(node) == NODE_MAX(node)) {
		error = fnode_create_bucket(node, 0, key, &bucket);
		if (error) {
			panic("Problem fetching");
		}

		fnode_setval(parent, at, &bucket->fn_location);
		parent->fn_pointers[at] = bucket;
		fnode_write(parent);

		return fnode_bucket_insert(parent, bucket, at, key, value);
	}

	fnode_setval(node, NODE_SIZE(node), value);
	node->fn_dnode->dn_numkeys += 1;

	return (0);
}

int
fnode_create_bucket(struct fnode *node, int at, void * key, struct fnode **final)
{
	int error;
	struct fnode *bucket;
	error = fbtree_allocnode(node->fn_tree, &bucket, BT_BUCKET);
	if (error) {
		panic("Problem with creating bucket");
		return (error);
	}

	MPASS(NODE_TYPE(bucket) == BT_BUCKET);
	node->fn_pointers[at] = bucket;
	if (NODE_TYPE(node) != BT_BUCKET) {
		MPASS(NODE_TYPE(node) == BT_EXTERNAL);
		node->fn_types[at] = BT_BUCKET;
		/* We have to take the value currently there and place it into 
		 * the bucket
		 */
		fnode_bucket_insert(node, bucket, at, key, fnode_getval(node, at));
		fnode_setval(node, at, &bucket->fn_location);
		fnode_setkey(bucket, 0, key);
		BUCKET_SETNEXT(bucket, NULL);
	} else {
		MPASS(NODE_SIZE(node) == NODE_MAX(node));
		// Set the last value to the node, we are adding buckets at the 
		// head of the linked list (for less page reads to find an 
		// empty bucket
		BUCKET_SETNEXT(bucket, &node->fn_location);
		BUCKET_SETPOINTER(bucket, node);
		KASSERT(at == 0, ("Buckets must place pointers at 0"));
	}
	*final = bucket;

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
	int index;
	bnode_ptr ptr;
	int compare;
	struct fnode *bucket = NULL;

	index = fnode_first_greater_equal(node, key);
	if (index < NODE_SIZE(node)) {
		keyt = fnode_getkey(node, index);
		compare = NODE_COMPARE(node, keyt, key);
	} else {
		compare = 1;
	}

	if (compare) {
		fnode_insert_at(node, key, value, index);
	} else {
		if (!(node->fn_tree->bt_flags & FN_ALLOWDUPLICATE)) {
			return (EINVAL);
		}

		if (NODE_ISBUCKETAT(node, index)) {
			ptr = *(bnode_ptr *)fnode_getval(node, index);
			error = fnode_fetch(node, index, &bucket);
			if (error) {
				panic("Problem fetching bucket");
			}
			MPASS(NODE_TYPE(bucket) == BT_BUCKET);
			error = fnode_bucket_insert(node, bucket, index, key, value);
			fnode_write(bucket);

			return (error);
		} else {
			error = fnode_create_bucket(node, index, key, &bucket);
			if (error) {
				panic("Problem creating bucket");
			}
			KASSERT(NODE_TYPE(bucket) == BT_BUCKET, ("Problem with creating bucket %u", NODE_TYPE(bucket)));
			// We set the key as we need to find the parent for 
			// when we dirty parts of the btree
			error = fnode_bucket_insert(node, bucket, index, key, value);
			fnode_write(bucket);

			return (error);
		}

		if (error) {
			panic("Problem with bucket");
		}
	}

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

	if (NODE_TYPE(node) == BT_INTERNAL) {
		error = fnode_internal_insert(node, key, value);
	} else if (NODE_TYPE(node) == BT_BUCKET) {
		panic("Should not be using fnode insert for buckets");
	} else {
		error = fnode_external_insert(node, key, value);
	}

	KASSERT(error == 0, ("error %d for inserting key %lu in %p",
	    error, *(uint64_t *) key, node));

	fnode_write(node);

	return (error);
}

/*
 * Remove a key-value pair from a node.
 */
static int
fnode_remove_at(struct fnode *node, void *value, int i)
{
	size_t ntail = NODE_SIZE(node) - i - 1;
	char *src = fnode_getkey(node, i);

	/* Scoot over the keys to the left, overwriting the one in position i.*/
	memmove(src, src + node->fn_tree->bt_keysize, ntail * NODE_KS(node));

	/* The offset of the corresponding value is different depending if the node is internal or not. */
	if (NODE_TYPE(node) == BT_INTERNAL) {
		src = fnode_getval(node, i + 1);
		memmove(src, src + NODE_VS(node), ntail * NODE_VS(node));
		memmove(&node->fn_pointers[i + 1], &node->fn_pointers[i + 2], ntail  * sizeof(node->fn_pointers[i]));
	} else {
		src = fnode_getval(node, i);
		if (value) {
			memcpy(value, src, node->fn_tree->bt_valsize);
		}
		memmove(src, src + NODE_VS(node), ntail  * NODE_VS(node));
		memmove(&node->fn_types[i], &node->fn_types[i + 1], ntail * sizeof(node->fn_types[i]));
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
	fnode_remove_at(node, value, i);

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

	if (NODE_TYPE(node) == BT_INTERNAL) {
		error = fnode_internal_remove(node, key, value);
	} else {
		error = fnode_external_remove(node, key,  value);
	}

	/* Check if the root changed. */
	if (ptr != node->fn_tree->bt_root) {
		fbtree_execrc(node->fn_tree);
	}

	/* Write out the node. */
	fnode_write(node);

	return (error);
}

/*
 * Safely remove a key-value pair while iterating over it.
 */
int
fiter_remove(struct fnode_iter *it)
{
	struct fnode *fnode = it->it_node;
	struct fnode *parent;
	size_t i;
	bnode_ptr *ptr;

	KASSERT(it->it_index < NODE_SIZE(fnode),
	        ("Removing an out-of-bounds iterator"));

	fnode_remove_at(fnode, NULL, it->it_index);
	fnode_iter_skip(it);

	// If we removed the last key from this node, remove the entire node
	// TODO: merging
	while (NODE_SIZE(fnode) == 0) {
		fnode_parent(fnode, &parent);
		if (parent == NULL) {
			break;
		}

		for (i = 0; i < NODE_SIZE(parent) + 1; ++i) {
			if (fnode == parent->fn_pointers[i]) {
				break;
			}
		}
		MPASS(i < NODE_SIZE(parent) + 1);

		ptr = fnode_getval(parent, i);
		MPASS(fnode->fn_location == *ptr);

		fnode_remove_at(parent, NULL, i);
		NODE_FREE(fnode);
		fnode = parent;
	}

	fnode_write(fnode);
	return (0);
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
	error = fnode_follow(tree->bt_rootnode, key, NULL, &node);
	if (error) {
		return (error);
	}

	/* Remove it from the node. */
	error = fnode_remove(node, key, value);
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
	error = fnode_follow(tree->bt_rootnode, key, NULL, &node);
	if (error) {
		DEBUG("Problem following node");
		return (error);
	}

	/* Add it to the node. */
	error = fnode_insert(node, key, value);

#ifdef INVARIANTS
	struct fnode_iter iter, next;
	int k = 0;
	int compare;
	fbtree_keymin_iter(tree, &k, &iter);
	next = iter;
	ITER_NEXT(next);
	while (!ITER_ISNULL(next)) {
		compare = NODE_COMPARE(next.it_node, ITER_KEY(next), ITER_KEY(iter));
		if (compare <= 0) {
			fnode_print(next.it_node);
			fnode_print(iter.it_node);
			panic("Tree is in improper state, keys to the right are not ascending %lu", ITER_KEY_T(iter, size_t));
		}
		iter = next;
		ITER_NEXT(next);
	}
#endif

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

void
fiter_replace(struct fnode_iter *it, void *val)
{
	memcpy(ITER_VAL(*it), val, it->it_node->fn_tree->bt_valsize);
}

/*
 * Dirty fnode by using a delayed write, this requires going up and dirtying 
 * the parent as well 
 */
void
fnode_write(struct fnode *node)
{
	KASSERT(BTREE_LKSTATUS(node->fn_tree) & (LK_EXCLUSIVE), ("Should be locked exclusively"));
	struct fnode *parent;
	while (node) {
		BUF_LOCK(node->fn_buf, LK_EXCLUSIVE, NULL);
		if ((node->fn_buf->b_flags & B_MANAGED) == 0) {
			node->fn_buf->b_flags |= B_CLUSTEROK | B_MANAGED;
			bremfree(node->fn_buf);
		}
		bdwrite(node->fn_buf);
		fnode_parent(node, &parent);
		node = parent;
	}
}

/*
 * Create an in-memory representation of a bnode.
 */
int
fnode_init(struct fbtree *tree, bnode_ptr ptr, struct fnode **fn)
{
	int error;
	struct fnode *node = NULL;

	if (*fn == NULL) {
		node = NODE_ALLOC(M_WAITOK);
	} else {
		node = *fn;
	}

#ifdef VERBOSE
	DEBUG2("fnode_init vp=%p ptr=%lu", tree->bt_backend, ptr);
#endif
	/* Read the data from the disk into the buffer cache. */
	error = fnode_getbufptr(tree, ptr, &node->fn_buf);
	if (error) {
		printf("Problem getting buf");
		return (error);
	}
	
	fnode_setup(node, tree, ptr);

	*fn = node;

	bqrelse(node->fn_buf);

	return (0);
}

int
fbtree_test(struct fbtree *tree)
{
	int error;
	int size = 2000 * 2;
	struct fnode_iter iter;
	tree->bt_flags = FN_ALLOWDUPLICATE;
	size_t *keys = (size_t *)malloc(sizeof(size_t) * size, M_SLOS_BTREE, M_WAITOK);

	srandom(1);

	BTREE_LOCK(tree, LK_EXCLUSIVE);
	for (int i = 0; i < size; i++) {
		keys[i] = random();
		size_t bigboi = i;
		error = fbtree_insert(tree, &keys[i], &bigboi);
		if (error) {
			DEBUG1("%d ERROR", error);
		}
		for (int t = 0; t < i; t++) {
			error = fbtree_keymin_iter(tree, &keys[t], &iter);
			if (error) {
				DEBUG1("%d ERROR", error);
				DEBUG1("KEY %lu", keys[t]);
				panic("Problem");
			}
			if (ITER_ISNULL(iter)) {
				DEBUG1("KEY %lu", keys[t]);
				fnode_print(iter.it_node);
				panic("Problem");
			}
			if (!ITER_ISBUCKETAT(iter)) {
				KASSERT(ITER_VAL_T(iter, size_t) == t, ("Test not equal"));
			}
		}
	}

	for (int i = 0; i < 1000; i++) {
		size_t bigboi = random();
		error = fbtree_insert(tree, &keys[500], &bigboi);
		if (error) {
			panic("Error");
		}
	}

	error = fbtree_keymin_iter(tree, &keys[500], &iter);
	MPASS(ITER_ISBUCKETAT(iter));
	ITER_NEXT(iter);
	int i = 0;
	MPASS(NODE_TYPE(iter.it_node) == BT_BUCKET);
	while (NODE_TYPE(iter.it_node) == BT_BUCKET) {
		size_t p = ITER_VAL_T(iter, size_t);
		printf("Value %lu", p);
		ITER_NEXT(iter);
		i++;
	}
	KASSERT(i == 1001, ("%d", i));

	for (size_t t = 0; t < size; t++) {
		if (t == 500) {
			continue;
		}
		error = fbtree_keymin_iter(tree, &keys[t], &iter);
		if (error) {
			DEBUG1("%d ERROR", error);
			DEBUG1("KEY %lu", keys[t]);
			panic("Problem");
		}
		if (ITER_ISNULL(iter)) {
			DEBUG1("KEY %lu", keys[t]);
			fnode_print(iter.it_node);
			panic("Problem");
		}
		if (ITER_VAL_T(iter, size_t) != t) {
			printf("%lu key, %lu", keys[t], t);
			printf("found %lu, %u", ITER_KEY_T(iter, size_t), iter.it_index);
			fnode_print(iter.it_node);
			panic("Test not equal");
		}
	}

	BTREE_UNLOCK(tree, 0);
	free(keys, M_SLOS_BTREE);

	return (0);
}

#ifdef SLOS_TEST

/* Constants for the testing function. */

#define KEYSPACE    50000 /* Number of keys used */
#define KEYINCR     100   /* Size of key range */
#define ITERATIONS  100000  /* Number of iterations */
#define CHECKPER    10000 /* Iterations between full tree checks */
#define VALSIZE     (sizeof(uint64_t)) /* Size of the btree's values */
#define POISON      'b'   /* Poison byte for the values. */

#define PINSERT     40    /* Weight of insert operation */
#define PDELETE     60    /* Weight of delete operation */
#define PSEARCH     20    /* Weight of search operation */
#define PKEYLIMIT   20    /* Weight of keymin/keymax operations */

#define FBTEST_ID   (0xabcdef)  /* OID of the fbtree vnode for testing. */

/*
 * Randomized test for the btree. At each round the program randomly
 * selects whether to insert, delete, or search for a random key.
 * By holding information whether the number should be found, and
 * checking the result of the operation, we can verify that each
 * operation properly succeeds or fails.
 *
 * Periodically, we also do full checks of the tree, traversing it
 * and verifying that its ordering and size invariants hold.
 */
int
slsfs_fbtree_test(void)
{

	struct fbtree *btree;
	struct fnode_iter iter;
	int error, i, j;
	uint64_t *keys;
	int *is_there, *was_there;
	int operation, index, key_present;
	uint64_t key, limkey;
	void *value;
	uint64_t minkey, maxkey;
	struct slos_node *svp;
	struct vnode *vp;
	void **values;
	uint64_t oid;

	oid = FBTEST_ID;
	error = slsfs_new_node(&slos, MAKEIMODE(VREG, S_IRWXU), &oid);
	if (error != 0) {
		printf("slsfs_new_node() failed with %d", error);
		return (error);
	}

	/* We just create a random vnode and grab its data tree for the test. */
	error = VFS_VGET(slos.slsfs_mount, oid, LK_EXCLUSIVE, &vp);
	if (error != 0) {
		printf("VFS_VGET() failed with %d", error);
		return (error);
	}

	svp = SLSVP(vp);
	btree = &svp->sn_tree;

	/*
	 * Our values are of arbitrary size, so create a random legible string
	 * of characters for each one. We reuse the buffer later.
	 */
	value = malloc(VALSIZE, M_SLOS_BTREE, M_WAITOK);


	/*
	 * One of the arrays is the actual numbers, the other
	 * checks whether the number is there, the third checks
	 * whether the number was ever in the btree. Since the numbers
	 * generated increase strictly monotonically, we don't
	 * need to worry about duplicates.
	 */
	keys = malloc(sizeof(*keys) * KEYSPACE, M_SLOS_BTREE, M_WAITOK);
	is_there = malloc(sizeof(*is_there) * KEYSPACE, M_SLOS_BTREE, M_WAITOK | M_ZERO);
	was_there = malloc(sizeof(*was_there) * KEYSPACE, M_SLOS_BTREE, M_WAITOK | M_ZERO);

	printf("Starting test");

	/*
	 * Begin from 1, because we'd like to keep 0 for invalid keys during
	 * tests (while testing the upper bound, key - 1 is used to denote an
	 * invalid upper bound. The dual of this for an upper bound on the
	 * values of the keys is to disallow the use of the value UINT64_MAX,
	 * but, provided KEYINCR * KEYSPACE is small relative to that value,
	 * there is no possibility of it arising.
	 */
	keys[0] = 1;
	for (i = 1; i < KEYSPACE; i++)
		keys[i] = keys[i - 1] + 1 + (random() % KEYINCR);

	values = malloc(sizeof(*values) * KEYSPACE, M_SLOS_BTREE, M_WAITOK);
	for (i = 0; i < KEYSPACE; i++) {
		values[i] = malloc(VALSIZE, M_SLOS_BTREE, M_WAITOK);
		for (j = 0; j < VALSIZE - 1; j++)
			((char *) values[i])[j] = 'a' + (random() % ('z' - 'a'));
		((char *) values[i])[VALSIZE - 1] = '\0';
	}

	for (i = 0; i < ITERATIONS; i++) {
		operation = random() % (PINSERT + PDELETE + PSEARCH + PKEYLIMIT);
		index = random() % (KEYSPACE - 1);

		key = keys[index];
		key_present = is_there[index];

		if (operation < PINSERT) {

			/* Value is irrelevant, so have it be the the key. */
			error = fbtree_insert(btree, &key, values[index]);
			if (error && !key_present) {
				printf("ERROR: Insertion of nonduplicate failed");
				goto out;
			} else if (error && key_present) {
				printf("ERROR: Insertion of duplicate succeeded");
				error = EINVAL;
				goto out;
			}

			error = fbtree_get(btree, &key, value);
			if (error) {
				printf("ERROR %d: Search of just inserted key %lx failed", error, key);
				goto out;
			}

			is_there[index] = 1;
			was_there[index] = 1;
		} else if (operation < PINSERT + PDELETE) {
			// XXX: Add tests when delete is supported
		} else if (operation < PINSERT + PDELETE + PSEARCH) {

			error = fbtree_get(btree, &key, value);
			if (error && key_present) {
				printf("ERROR: Search of existing key failed");
				goto out;
			} else if (!error && !key_present) {
				printf("ERROR: Search of nonexistent key succeeded");
				error = EINVAL;
				goto out;
			}

			if (!error && (memcmp(value, values[index], VALSIZE) != 0)) {
				printf("ERROR: Value %.*s not equal to expected %.*s",
				    (int) VALSIZE, (char *) value, (int) VALSIZE, (char *) values[index]);
				error = EINVAL;
				goto out;

			}

		} else {
			limkey = key;

			/*
			 * Try to find the lower bound of the key.
			 * The initial value is a sentinel that shows
			 * the key is smaller than all others in the tree.
			 */
			minkey = key + 1;
			for (j = index; j >= 0; j--) {
				if (is_there[j]) {
					minkey = keys[j];
					break;
				}
			}

			error = fbtree_keymin_iter(btree, &limkey, &iter);
			if (error != 0) {
				printf("ERROR: %d when finding lower bound for %lu", error, key);
				goto out;
			}

			if (ITER_ISNULL(iter)) {
				if (minkey <= key) {
					printf("ERROR: Lower bound %lu for key %lu not found", minkey, key);
					error = EINVAL;
					goto out;
				}
			} else {
				limkey = ITER_KEY_T(iter, uint64_t);
				if (minkey != ITER_KEY_T(iter, uint64_t)) {
					printf("ERROR: Lower bound for key %lu should be %lu, is %lu",
					       key, minkey, limkey);
					error = EINVAL;
					goto out;
				}
			}

			/*
			 * Do the dual of the above
			 * operations to find the upper bound.
			 */
			limkey = key;
			maxkey = key - 1;
			for (j = index; j < KEYSPACE; j++) {
				if (is_there[j]) {
					maxkey = keys[j];
					break;
				}
			}

			error = fbtree_keymax_iter(btree, &limkey, &iter);
			if (error != 0) {
				printf("ERROR: %d when finding upper bound for %lu", error, key);
				goto out;
			}

			if (ITER_ISNULL(iter)) {
				if (maxkey >= key) {
					printf("ERROR: Upper bound %lu for key %lu not found", maxkey, key);
					error = EINVAL;
					goto out;
				}
			} else {
				limkey = ITER_KEY_T(iter, uint64_t);
				if (maxkey != limkey) {
					printf("ERROR: Upper bound for key %lu should be %lu, is %lu",
					       key, maxkey, limkey);
					limkey = maxkey;
					error = EINVAL;
					goto out;
				}

			}

		}

	}

	error = 0;
out:
	printf("Iterations: %d", i);

	for (i = 0; i < KEYSPACE; i++)
		free(values[i], M_SLOS_BTREE);
	free(values, M_SLOS_BTREE);

	free(value, M_SLOS_BTREE);

	free(keys, M_SLOS_BTREE);
	free(is_there, M_SLOS_BTREE);
	free(was_there, M_SLOS_BTREE);

	vput(vp);
	/* XXX Destroy the on-disk node created when we have reclamation. */

	return error;
}

#endif /* SLOS_TEST */

#ifdef DDB
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
	if (NODE_TYPE(node) == BT_INTERNAL) {
		bnode_ptr *p = fnode_getval(node, 0);
		db_printf("| C %lu |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = fnode_getval(node, i + 1);
			size_t *t = fnode_getkey(node, i);
			db_printf("| K %lu || C %lu |", *t, *p);
		}
		db_printf("Child memory\n");
		for (int i = 0; i <= NODE_SIZE(node); i++) {
			if (node->fn_pointers[i] != NULL) {
				struct fnode *n = (struct fnode *)node->fn_pointers[i];
				db_printf("| node(%lu) |", n->fn_location);
			} else {
				db_printf("| NULL |");
			}
		}

	} else {
		for (i = 0; i < NODE_SIZE(node); i++) {
			uint64_t *t = fnode_getkey(node, i);
			diskptr_t *v = fnode_getval(node, i);
			db_printf("| %lu -> %lu, %lu |,", *t, v->offset, v->size);
		}
	}
	db_printf("\n");
}
#endif
