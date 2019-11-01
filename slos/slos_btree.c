#include <sys/param.h>

#include <sys/queue.h>
#include <sys/systm.h>

#include "slos_alloc.h"
#include "slos_bootalloc.h"
#include "slos_btree.h"
#include "slos_internal.h"
#include "slos_io.h"

#include "slosmm.h"

/* Initialize an in-memory btree. */
struct btree *
btree_init(struct slos *slos, uint64_t blkno, int alloctype)
{
	struct btree *btree;

	btree = malloc(sizeof(*btree), M_SLOS, M_WAITOK | M_ZERO);
	btree->size = 0;
	btree->depth = 1;
	btree->alloctype = alloctype;
	LIST_INIT(&btree->btreeq);

	btree->root = blkno;

	return btree;
}

static uint64_t 
btree_blkget(struct slos *slos,struct btree *btree)
{
	struct slos_diskptr diskptr;

	if (btree->alloctype == ALLOCBOOT)
	    diskptr = slos_bootalloc(slos->slos_bootalloc);
	else
	    diskptr = slos_alloc(slos->slos_alloc, 1);

	if (diskptr.offset== 0)
	    return 0;

	return diskptr.offset;
}

static void 
btree_blkput(struct slos *slos, struct btree *btree, uint64_t blkno)
{
	if (btree->alloctype == ALLOCBOOT)
	    slos_bootfree(slos->slos_bootalloc, DISKPTR_BLOCK(blkno));
	else
	    slos_free(slos->slos_alloc, DISKPTR_BLOCK(blkno));
}

/* 
 * Add a bnode to a list of all the bnodes that won't
 * be needed if an operation succeeds.
 */
void
btree_addelem(struct btree *btree, struct bnode *bnode)
{
	struct belem *belem; 
	
	belem = malloc(sizeof(*belem), M_SLOS, M_WAITOK);
	belem->bnode = bnode;
	LIST_INSERT_HEAD(&btree->btreeq, belem, entries);
}

/* 
 * An operation succeeded, free both in-memory and
 * on-disk resources for the bnodes added to the queue.
 */
void
btree_discardelem(struct btree *btree)
{
	struct belem *cur, *tmp;

	LIST_FOREACH_SAFE(cur, &btree->btreeq, entries, tmp) {
	    LIST_REMOVE(cur, entries);
	    btree_blkput(&slos, btree, cur->bnode->blkno);
	    bnode_free(cur->bnode);
	    free(cur, M_SLOS);
	}
}

/* 
 * An operation failed, free both only in-memory
 * resources for the bnodes added to the queue.
 */
void
btree_keepelem(struct btree *btree)
{
	struct belem *cur, *tmp;

	LIST_FOREACH_SAFE(cur, &btree->btreeq, entries, tmp) {
	    LIST_REMOVE(cur, entries);
	    bnode_free(cur->bnode);
	    free(cur, M_SLOS);
	}
}

/* Release an in-memory btree's resources. */
void
btree_destroy(struct btree *btree)
{
	/* 
	 * Err on the side of caution, 
	 * keep the elements on disk. 
	 */
	btree_keepelem(btree);
	free(btree, M_SLOS);
}

/*
 * Clear flags pertaining to the last 
 * operation. Called when doing a new 
 * insert/delete/search.
 */
static void 
btree_clearflags(struct btree *btree)
{
	btree->op_caused_merge = 0;
	btree->op_caused_borrow = 0;
	btree->op_caused_split = 0;
}

/* Print statistics for the btree. */
void
btree_print(struct btree *btree)
{
    printf("Btree pointer: %p\n", btree);
    printf("Size: %lu\tDepth: %lu\n", btree->size, btree->depth);
    printf("Last op %hhu Caused borrow: %d Caused merge: %d Caused split: %d\n", 
	    btree->last_op, btree->op_caused_borrow, btree->op_caused_merge,
	    btree->op_caused_split);
    printf("Inserts: %lu\tDeletes: %lu\tSearches: %lu\t\n", btree->inserts,
	    btree->deletes, btree->searches);
    printf("Overwrites: %lu\tKeymin: %lu\tKeymaxes: %lu\t\n", btree->overwrites,
	    btree->keymins, btree->keymaxes);
    printf("Splits: %lu\tMerges: %lu\tBorrows: %lu\t\n", btree->splits,
	    btree->merges, btree->borrows);

}

/* Check if a btree is empty. */
int
btree_empty(struct btree *btree, int *is_empty)
{
	struct bnode *broot;
	int error;

	error = bnode_read(&slos, btree->root, &broot);
	if (error != 0)
	    return error;

	*is_empty = ((broot->external == BNODE_EXTERNAL) && (broot->size == 0));

	bnode_free(broot);

	return 0;
}

/*
 * Return the lowest key/value pair in the btree.
 */
int
btree_first(struct btree *btree, uint64_t *key, void *value)
{
	struct bnode *bnode, *bparent;
	int error = 0;

	error = bnode_read(&slos, btree->root, &bnode);
	if (error != 0)
	    return error;

	bparent = bnode;
	while (bnode->external == BNODE_INTERNAL) {
	    bnode = bnode_child(bnode, 0);
	    if (bnode == NULL) {
		bnode_free(bparent);
		return EINVAL;
	    }

	    /* Lazy update of parent pointers in the path. */
	    if (bnode->parent.offset != bparent->blkno) {
		bnode->parent.offset = bparent->blkno;
		error = bnode_write(&slos, bnode);
		if (error != 0) {
		    bnode_free(bparent);
		    error = EIO;
		    goto out;
		}
	    }

	    /* Free the parent pointer (if it's not the root) */
	    bnode_free(bparent);
	    bparent = bnode;
	}

	/* Empty tree */
	if (bnode->size == 0) {
	    bnode_free(bnode);
	    return EINVAL;
	}

	error = bnode_getkey(bnode, 0, key);
	if (error != 0)
	    goto out;

	error = bnode_getvalue(bnode, 0, value);
	if (error != 0)
	    goto out;

out:
	/* Free the in-memory bnode. */
	bnode_free(bnode);

	return error;

}
/*
 * Return the external node possibly containing the key.
 */
static struct bnode *
btree_candidate(struct btree *btree, uint64_t key)
{
	struct bnode *bnode, *bparent;
	uint64_t bkey;
	size_t nextoff;
	int error;
	
	error = bnode_read(&slos, btree->root, &bnode);
	if (error != 0)
	    return NULL;

	while (bnode->external == BNODE_INTERNAL) {

	    /* Traverse the keys in increasing order. */
	    for (nextoff = 0; nextoff < bnode->size; nextoff++) {
		bnode_getkey(bnode, nextoff, &bkey);
		if (key <= bkey)
		    break;
	    }

	    /* Follow the path to the key. */
	    bparent = bnode;
	    bnode = bnode_child(bnode, nextoff);
	    if (bnode == NULL) {
		bnode_free(bparent);
		return NULL;
	    }

	    /* Lazy update of parent pointers in the path. */
	    if (bnode->parent.offset != bparent->blkno) {
		bnode->parent.offset = bparent->blkno;
		error = bnode_write(&slos, bnode);
		if (error != 0) {
		    bnode_free(bnode);
		    bnode_free(bparent);
		    return NULL;
		}
	    }

	    /* 
	     * If the previous node is 
	     * not the root, free it.
	     */
	    bnode_free(bparent);
	}

	return bnode;
}

/* Finds the largest key in the tree that's smaller than or equal to the argument. */
int 
btree_keymin(struct btree *btree, uint64_t *key, void *value)
{
	struct bnode *bnode, *bparent, *bchild;
	uint64_t bkey;
	size_t boffset;
	int error = 0, i;

	/* Get the only node that could contain the key. */
	bnode = btree_candidate(btree, *key);
	if (bnode == NULL)
	    return EIO;

	/* If the btree has size 0, then there are no keys.*/
	if (bnode->size == 0) {
	    bnode_free(bnode);
	    return EINVAL;
	}

	/* 
	 * If all keys in the node are larger than the argument,
	 * go upwards until we find a subtree to the left of the
	 * one we're currently in. The largest key in that subtree
	 * is what we're looking for.
	 */
	error = bnode_getkey(bnode, 0, &bkey);
	if (error != 0)
	    return error;

	if (bkey > *key) {
	    do {
		/* 
		 * If we're at the root and still 
		 * haven't found a subtree to the
		 * left, there is no key smaller than
		 * the argument. Return failure.
		 */
		if (bnode->blkno == bnode->parent.offset) {
		    error = EINVAL;
		    goto out;
		}

		/* 
		 * Guaranteed to succeed, since we
		 * traversed the path from the root
		 * to the bnode, fixing up any
		 * stale parent pointers.
		 */
		bparent = bnode_parent(bnode);
		if (bparent == NULL) {
		    error = EIO;
		    goto out;
		}

		boffset = bnode_parentoff(bnode, bparent);

		bnode_free(bnode);
		bnode = bparent;

	    } while (boffset == 0);

	    /* 
	     * We found a subtree to the left. Traverse
	     * down the right side, finding the largest key.
	     * Here we don't fix any stale parent pointers,
	     * because we won't need them for this operation.
	     */
	    bchild = bnode_child(bnode, boffset - 1);
	    if (bchild == NULL) {
		error = EIO;
		goto out;
	    }

	    bnode_free(bnode);
	    bnode = bchild;

	    /* Go down the right side of the subtree. */
	    while (bnode->external == BNODE_INTERNAL) {
		bchild = bnode_child(bnode, bnode->size);
		if (bchild == NULL) {
		    error = EIO;
		    goto out;
		}

		bnode_free(bnode);
		bnode = bchild;
	    }

	    /* Get the key and value pair. */
	    error = bnode_getkey(bnode, bnode->size - 1, key);
	    if (error != 0)
		goto out;

	    error = bnode_getvalue(bnode, bnode->size - 1, value);
	    if (error != 0)
		goto out;

	} else {
	    /* 
	    * Otherwise search the node for the largest key
	    * that's still smaller than the argument.
	    */
	    for (i = bnode->size - 1; i >= 0; i--) {
		error = bnode_getkey(bnode, i, &bkey);
		if (error != 0)
		    goto out;

		if (*key >= bkey) {
		    *key = bkey;

		    error = bnode_getvalue(bnode, i, value);
		    if (error != 0)
			goto out;

		    break;
		}
	    }
	}

	btree_clearflags(btree);

	btree->last_op = OPKEYMIN;
	btree->keymins += 1;

out:
	bnode_free(bnode);

	return error;
}

/* Finds the smallest key in the tree that's larger than or equal to the argument. */
int 
btree_keymax(struct btree *btree, uint64_t *key, void *value)
{
	struct bnode *bnode, *bparent, *bchild;
	uint64_t bkey;
	size_t boffset;
	int error = 0, i;
 

	/* Get the only node that could contain the key. */
	bnode = btree_candidate(btree, *key);
	if (bnode == NULL)
	    return EIO;

	/* If the btree has size 0, then there are no keys.*/
	if (bnode->size == 0) {
	    bnode_free(bnode);
	    return EINVAL;
	}

	/* 
	 * If all keys in the node are smaller than the argument,
	 * go upwards until we find a subtree to the right of the
	 * one we're currently in. The smallest key in that subtree
	 * is what we're looking for.
	 */
	error = bnode_getkey(bnode, bnode->size - 1, &bkey);
	if (error != 0)
	    return EINVAL;

	if (bkey < *key) {
	    do {
		/* 
		 * If we're at the root and still 
		 * haven't found a subtree to the
		 * right, there is no key smaller than
		 * the argument. Return failure. Don't
		 * try to free the bnode, since it's
		 * the root.
		 */
		if (bnode->blkno == bnode->parent.offset) {
		    error = EINVAL;
		    goto out;
		}

		bparent = bnode_parent(bnode);
		if (bparent == NULL) {
		    error = EIO;
		    goto out;
		}

		boffset = bnode_parentoff(bnode, bparent);

		bnode_free(bnode);
		bnode = bparent;


	    } while (boffset == bnode->size);

	    /* 
	     * We found a subtree to the right. Traverse
	     * down the left side, finding the smallest key.
	     * Here we don't fix any stale parent pointers,
	     * because we won't need them for this operation.
	     */
	    bchild = bnode_child(bnode, boffset + 1);
	    if (bchild == NULL) {
		error = EIO;
		goto out;
	    }

	    bnode_free(bnode);
	    bnode = bchild;

	    /* Go down the left subtere. */
	    while (bnode->external == BNODE_INTERNAL) {
		bchild = bnode_child(bnode, 0);
		if (bchild == NULL) {
		    error = EIO;
		    goto out;
		}

		bnode_free(bnode);
		bnode = bchild;
	    }

	    error = bnode_getkey(bnode, 0, key);
	    if (error != 0)
		goto out;

	    error = bnode_getvalue(bnode, 0, value);
	    if (error != 0)
		goto out;


	} else {
	    /* 
	    * Otherwise search the node for the smallest key
	    * that's still larger than the argument.
	    */
	    for (i = 0; i < bnode->size; i++) {
		error = bnode_getkey(bnode, i, &bkey);
		if (error != 0)
		    goto out;

		if (*key <= bkey) {
		    *key = bkey;

		    error = bnode_getvalue(bnode, i, value);
		    if (error != 0)
			goto out;

		    break;
		}
	    }
	}

	btree_clearflags(btree);

	btree->last_op = OPKEYMAX;
	btree->keymaxes += 1;

out:
	bnode_free(bnode);

	return error;
}

int
btree_search(struct btree *btree, uint64_t key, void *value)
{
	struct bnode *bnode;
	int error;

	/* Get the only node that could contain the key. */
	bnode = btree_candidate(btree, key);
	if (bnode == NULL)
	    return EIO;

	/* Search into the bnode for the key. */
	error = bnode_search(bnode, key, value);
	if (error == 0) {
	    /* If successful, update statistics. */
	    btree_clearflags(btree);
	    btree->last_op = OPSEARCH;
	    btree->searches += 1;
	}

	bnode_free(bnode);

	return error;
}

/*
 * Insertion related functions.
 */

static int
bnode_split(struct btree *btree, struct bnode **bnode)
{
	struct bnode *bright = NULL, *bleft = NULL;
	struct bnode *bparent = NULL, *broot = NULL;
	struct slos_diskptr bptr;
	uint64_t bkey;
	int error = 0, i;
	daddr_t blkleft = 0, blkright = 0, blkroot = 0;
	uint64_t key;

	/* 
	 * In order for modifications to the 
	 * btree to be atomic, we can't modify
	 * the old block in place. If we did, then
	 * if we overwrote the old value but crashed
	 * before updating the parent, we would lose 
	 * half the keys in the node. We therefore make
	 * a new bnode which will replace the old one
	 * in the tree.
	 */

	/* 
	 * Allocate on-disk blocks for 
	 * any bnodes we might create.
	 */
	blkleft = btree_blkget(&slos, btree);
	if (blkleft == 0) {
	    error = ENOSPC;
	    goto error;
	}

	blkright = btree_blkget(&slos, btree);
	if (blkright == 0) {
	    error = ENOSPC;
	    goto error;
	}

	blkroot = btree_blkget(&slos, btree);
	if (blkroot == 0) {
	    error = ENOSPC;
	    goto error;
	}

	/* 
	 * Create the bnodes that will 
	 * replace the original. 
	 */
	bleft = bnode_copy(&slos, blkleft, *bnode);
	if (bleft == NULL) {
	    error = EIO;
	    goto error;
	}

	bright = bnode_alloc(&slos, blkright, (*bnode)->vsize, (*bnode)->external);
	if (bright == NULL) {
	    error = EIO;
	    goto error;
	}
	bright->parent = (*bnode)->parent;

	broot = bnode_alloc(&slos, blkroot, (*bnode)->vsize, BNODE_INTERNAL);
	if (broot == NULL) {
	    error = EIO;
	    goto error;
	}
	broot->parent.offset = broot->blkno;
	broot->size = 1;



	if (bright->external == BNODE_EXTERNAL) {

	    /* The keys are split equally among the new nodes. */
	    bright->size = (*bnode)->size / 2;

	    /* Truncate the bleft, removing the upper keys. */
	    bleft->size = (*bnode)->size - bright->size;

	    /* 
	     * We can simply pick the last key of *bnode as the
	     * separator between it and bright, because the
	     * nodes are external. 
	     */
	    error = bnode_getkey(*bnode, bleft->size - 1, &key);
	    if (error != 0)
		goto error;

	    error = bnode_copydata(bright, *bnode, 0, bleft->size, bright->size);
	    if (error != 0)
		goto error;


	} else {

	    /*
	     * Because the nodes are internal, for k keys we have
	     * k + 1 children. If we therefore split the node into
	     * two new ones with m and n keys, we have to have
	     * (m + 1) + (n + 1) = k + 1 <=> m + n = k - 1.
	     */

	    bright->size = (*bnode)->size / 2;

	    /* Truncate the bleft, removing the upper keys. */
	    bleft->size = (*bnode)->size - bright->size - 1;

	    /* The key that will be added to the common parent. */
	    bnode_getkey(*bnode, bleft->size, &key);

	    /* 
	    * We assume the btree has BSIZE > 2. 
	    * Now that we split the node, and since each 
	    * internal node holds n + 1 children for n keys,
	    * that means that we need one less key across both nodes.
	    */
	    error = bnode_copydata(bright, *bnode, 0, bleft->size + 1, bright->size + 1);
	    if (error != 0)
		goto error;

	    /* 
	     * Normally, since we are transferring _bnodes_ to the
	     * new node, we should be updating the children's parent
	     * pointer. That would lead to a lot of IO on the spot,
	     * however, so we instead let the pointers be stale until
	     * we traverse a path that includes the children. Since
	     * to do that we pass through bright, we then lazily fix
	     * the parent pointer. 
	     */
	}

	/* If we are splitting the root, create a new one. */
	if ((*bnode)->parent.offset == (*bnode)->blkno) {

	    bnode_putkey(broot, 0, key);
	    bnode_putptr(broot, 0, DISKPTR_BLOCK(bleft->blkno));
	    bnode_putptr(broot, 1, DISKPTR_BLOCK(bright->blkno));

	    bleft->parent.offset = broot->blkno;
	    bright->parent.offset = broot->blkno;

	    bparent = broot;

	} else {

	    /* We didn't need to create a new root. */
	    bnode_free(broot);
	    btree_blkput(&slos, btree, blkroot);

	    /* Attach the new node to the parent */
	    bparent = bnode_parent(*bnode);
	    if (bparent == NULL) {
		error = EIO;
		goto error;
	    }

	    for (i = 0; i < bparent->size + 1; i++) {
		bnode_getptr(bparent, i, &bptr);
		if (bptr.offset == (*bnode)->blkno) {
		    bnode_shiftr(bparent, i + 1, 1);

		    /* 
		     * Use bnodes's key for bright, 
		     * and generate a new one for bold.
		     */
		    bnode_getkey(bparent, i, &bkey);
		    bnode_putkey(bparent, i + 1, bkey);
		    bnode_putptr(bparent, i + 1, DISKPTR_BLOCK(bright->blkno));

		    /* Remove bold from the tree, replace with bleft. */
		    bnode_putkey(bparent, i, key);
		    bnode_putptr(bparent, i, DISKPTR_BLOCK(bleft->blkno));

		    break;
		}
	    }

	    bleft->parent.offset = bparent->blkno;
	    bright->parent.offset = bparent->blkno;

	}

	/* 
	 * Write both new bnodes to disk. 
	 * Note that we haven't written
	 * out the parent; if we crash 
	 * before we do, we lose the 
	 * operation but keep the
	 * btree consistent. The old
	 * bnode that was split is
	 * left untouched.
	 */
	error = bnode_write(&slos, bleft);
	if (error != 0) {
	    error = EIO;
	    goto error;
	}
	error = bnode_write(&slos, bright);
	if (error != 0) {
	    error = EIO;
	    goto error;
	}

	bnode_free(bleft);
	bnode_free(bright);

	/* 
	 * Add the old bnode to a list for deletion if the split succeeds.
	 * We can't remove it yet because we haven't written out the
	 * parent, so we could still have an error cancel the
	 * transaction. 
	 */
	btree_addelem(btree, *bnode);
	*bnode = bparent;

	return 0;

error:
	/* 
	 * Something went wrong, free all
	 * resources allocated for the split. 
	 */
	if (blkleft != 0)
	    btree_blkput(&slos, btree, blkleft);

	if (blkright != 0)
	    btree_blkput(&slos, btree, blkright);

	if (blkroot != 0)
	    btree_blkput(&slos, btree, blkroot);

	bnode_free(bleft);
	bnode_free(bright);
	bnode_free(broot);

	return error;
}

/* 
 * Common code for btree_insert() and btree_overwrite(). Given a btree, insert
 * a key-value pair, returning the old value of the key if needed. If there is
 * nowhere to store the old value (oldval is NULL), and such a value exists,
 * then return an error.
 */
static int
btree_add(struct btree *btree, uint64_t key, void *value, void *oldval)
{
	struct bnode *bnode;
	uint64_t bkey;
	int error = 0, i;

	/* Get the only node that could contain the key. */
	bnode = btree_candidate(btree, key);
	if (bnode == NULL)
	    return EIO;

	for (i = 0; i < bnode->size; i++) {
	    bnode_getkey(bnode, i, &bkey);

	    if (key == bkey) {
		/* 
		 * If oldval is NULL, we were called
		 * from btree_insert(), so we do not
		 * accept insertions of existing keys.
		 */
		if (oldval == NULL) {
		    bnode_free(bnode);
		    return EINVAL;
		}

		/* 
		 * Otherwise we write the old value
		 * out, put the new value in, and 
		 * return. Since we do not change
		 * the size of the bnode, this case
		 * is very simple.
		 */
		bnode_getvalue(bnode, i, oldval);
		bnode_putvalue(bnode, i, value);
		bnode_write(&slos, bnode);
		bnode_free(bnode);

		btree->last_op = OPOVERWRITE;
		btree->overwrites += 1;

		return 0;
	    }

	    if (key < bkey)
		break;
	}

	/* Make room for the new value, and write it in. */
	bnode_shiftr(bnode, i, 1);
	bnode_putkey(bnode, i, key);
	bnode_putvalue(bnode, i, value);


	/* 
	 * If the node gets full, we need to split it.
	 * This means that the parent gets one more child,
	 * which in turn means that it may need to be split
	 * itself. We traverse the tree upwards after each split,
	 * splitting further as long as we need.
	 *
	 * Because the size field counts _keys_, but for internal
	 * nodes n keys correspond to n + 1 children, we have to 
	 * take into account whether a node is internal or not 
	 * in our checks. 
	 */
	while ((bnode->external == BNODE_EXTERNAL && bnode->size > bnode->bsize) || 
	       (bnode->external == BNODE_INTERNAL && bnode->size > bnode->bsize - 1)) {
	    error = bnode_split(btree, &bnode);
	    if (error != 0) {
		bnode_free(bnode);
		return error;
	    }

	    btree_clearflags(btree);
	    btree->op_caused_split = 1;
	    btree->splits += 1;

	}

	/* 
	 * If the bnode is external, we did not do any
	 * splits, so just write the value.
	 *
	 * Else we exited the loop, so the parent is properly. 
	 * sized. Overwrite the old value, and, if the parent
	 * is a new root, update the superblock.
	 */
	error = bnode_write(&slos, bnode);
	if (error != 0) {
	    bnode_free(bnode);
	    return error;
	}

	/* If we created a new root, update the parent to reflect that. */
	if (bnode->parent.offset == bnode->blkno && btree->root != bnode->blkno) {
	    btree->root = bnode->blkno;
	    btree->depth += 1;
	}

	/* 
	 * A note: Normally, we dould like to have the add and delete 
	 * operations do their memory management internally, which in
	 * this case means calling btree_discardlem() to get rid of 
	 * any bnodes not used anymore. However, the new btree roots
	 * often need to be saved externally in an arbitrary way. For
	 * example, the allocation btrees have their roots written on
	 * the superblock, while record btrees have their root at the
	 * inode.
	 *
	 * If we released past bnodes in here, without completing the
	 * external save, then if the external save later failed we
	 * would cause inconsistencies. Moreover, due to the arbitrary
	 * nature of the save operation, we have no way of doing it
	 * in this function (same for delete). We thus have no choice
	 * but to have the caller manually discard past bnodes with
	 * a call to btree_discardelem() after changing any external
	 * block pointers to point to the new bnode.
	 */


	bnode_free(bnode);

	btree->last_op = OPINSERT;
	btree->inserts += 1;
	btree->size += 1;

	return 0;
}

/* Write a key/value pair to the btree, returning the old value if it exists. */
int
btree_overwrite(struct btree *btree, uint64_t key, void *value, void *oldval)
{
    if (oldval == NULL)
	return EINVAL;

    return btree_add(btree, key, value, oldval);
}

/* Write a key/value pair to the btree. The key must not already exist. */
int
btree_insert(struct btree *btree, uint64_t key, void *value)
{
    return btree_add(btree, key, value, NULL);
}


/*
 * Deletion related functions.
 */
static int
bnode_borrow(struct btree *btree, struct bnode **bnode)
{
	struct bnode *bleft = NULL, *bright = NULL, *bparent = NULL;
	struct bnode *bleftcopy = NULL, *brightcopy = NULL;
	daddr_t blkleft = 0, blkright = 0;
	struct slos_diskptr bptr;
	uint64_t bkey, key;
	int pickedleft;
	size_t boffset;
	void *bval = NULL;

	int error = 0;

	KASSERT(2 * bnode->size < bnode->bsize, "bnode needs to borrow");


	bval = malloc((*bnode)->vsize, M_SLOS, M_WAITOK);

	blkleft = btree_blkget(&slos, btree);
	if (blkleft == 0) {
	    error = ENOSPC;
	    goto error;
	}

	blkright = btree_blkget(&slos, btree);
	if (blkright == 0) {
	    error = ENOSPC;
	    goto error;
	}

	bparent = bnode_parent(*bnode);
	if (bparent == NULL) {
	    error = EIO;
	    goto error;
	}
	
	/* 
	 * Find our position in the parent node. If nodes need to
	 * be large, this should be replaced with a binary search
	 * routine.
	 */
	boffset = bnode_parentoff(*bnode, bparent);

	KASSERT(boffset < bnode->size + 1, "bnode found");

	if (boffset > 0) {
	    bleft = bnode_child(bparent, boffset - 1);
	    if (bleft == NULL) {
		error = EIO;
		goto error;
	    }
	}

	if (boffset < bparent->size) {
	    bright = bnode_child(bparent, boffset + 1);
	    if (bright == NULL) {
		error = EIO;
		goto error;
	    }
	}

	/* 
	 * Try to borrow from the left node first. The node needs to
	 * exist, and borrowing should not break the btree invariant.
	 */
	if (bleft != NULL && 
		((bleft->external != 0 && (2 * bleft->size - 1) > bleft->bsize) ||
		((bleft->external == 0) && (2 * bleft->size + 1) > bleft->bsize))) {

	    /* Save the direction from which we're borrowing. */
	    pickedleft = 1;

	    bleftcopy = bnode_copy(&slos, blkleft, bleft);
	    if (bleftcopy == NULL) {
		error = EIO;
		goto error;
	    }
	    bleftcopy->parent.offset = bparent->blkno;
	    
	    brightcopy = bnode_copy(&slos, blkright, *bnode);
	    if (brightcopy == NULL) {
		error = EIO;
		goto error;
	    }
	    brightcopy->parent.offset = bparent->blkno;

	    /* The new key for the parent. Because ranges are right 
	     * limit inclusive, get the second to last key as the
	     * limit in case of external nodes.
	     */
	    if (brightcopy->external != 0)
		bnode_getkey(bleftcopy, bleftcopy->size - 2, &key);
	    else
		bnode_getkey(bleftcopy, bleftcopy->size - 1, &key);

	    /* Add room for the entry in the node. */
	    bnode_shiftr(brightcopy, 0, 1);

	    if (brightcopy->external == BNODE_EXTERNAL) {
		bnode_getkey(bleftcopy, bleftcopy->size - 1, &bkey);
		bnode_putkey(brightcopy, 0, bkey);
		
		bnode_getvalue(bleftcopy, bleftcopy->size - 1, bval);
		bnode_putvalue(brightcopy, 0, bval);

	    } else {
		bnode_getkey(bparent, boffset - 1, &bkey);
		bnode_putkey(brightcopy, 0, bkey);
		
		bnode_getptr(bleftcopy, bleftcopy->size, &bptr);
		bnode_putptr(brightcopy, 0, bptr);
	    }

	    /* No need for a memmove, we just reduce the size. */
	    bleftcopy->size -= 1;


	} else if (bright != NULL && 
		((bright->external != 0 && (2 * bright->size - 1) > bright->bsize) ||
		((bright->external == 0) && (2 * bright->size + 1) > bright->bsize))) {

	    /* Save the direction from which we're borrowing. */
	    pickedleft = 0;

	    /*
	    * If we couldn't borrow left, try to borrow right.
	    */

	    bleftcopy = bnode_copy(&slos, blkleft, *bnode);
	    if (bleftcopy == NULL) {
		error = EIO;
		goto error;
	    }
	    bleftcopy->parent.offset = bparent->blkno;
	    
	    brightcopy = bnode_copy(&slos, blkright, bright);
	    if (brightcopy == NULL) {
		error = EIO;
		goto error;
	    }
	    brightcopy->parent.offset = bparent->blkno;

	    /* The new key for the parent. */
	    bnode_getkey(brightcopy, 0, &key);

	    /* No need for a memmove, we just increase the size. */
	    bleftcopy->size += 1;

	    if (bleftcopy->external == BNODE_EXTERNAL) {
		bnode_getkey(brightcopy, 0, &bkey);
		bnode_putkey(bleftcopy, bleftcopy->size - 1, bkey);
		
		bnode_getvalue(brightcopy, 0, bval);
		bnode_putvalue(bleftcopy, bleftcopy->size - 1, bval);

	    } else {
		bnode_getkey(bparent, boffset, &bkey);
		bnode_putkey(bleftcopy, bleftcopy->size - 1, bkey);
		
		bnode_getptr(brightcopy, 0, &bptr);
		bnode_putptr(bleftcopy, bleftcopy->size, bptr);
	    }

	    /* Remove the first entry from the right node. */
	    bnode_shiftl(brightcopy, 0, 1);



	} else {

	    /* 
	     * We didn't borrow. The delete routine will find that the 
	     * node still needs rebalancing, and will initiate merging.
	     */

	    if (blkleft != 0)
		btree_blkput(&slos, btree, blkleft);

	    if (blkright != 0)
		btree_blkput(&slos, btree, blkright);

	    bnode_free(bleft);
	    bnode_free(bright);
	    bnode_free(bparent);
	    free(bval, M_SLOS);

	    return EINVAL;

	}

	/* write the new copies. */
	error = bnode_write(&slos, bleftcopy);
	if (error != 0)
	    goto error;

	error = bnode_write(&slos, brightcopy);
	if (error != 0)
	    goto error;

	/* 
	 * Modify the parent to point to the new copies. Also 
	 * update the parent's keys to keep the invariants. 
	 */
	if (pickedleft) {
	    bnode_putptr(bparent, boffset - 1, DISKPTR_BLOCK(bleftcopy->blkno));
	    bnode_putptr(bparent, boffset, DISKPTR_BLOCK(brightcopy->blkno));
	    bnode_putkey(bparent, boffset - 1, key);
	} else {
	    bnode_putptr(bparent, boffset, DISKPTR_BLOCK(bleftcopy->blkno));
	    bnode_putptr(bparent, boffset + 1, DISKPTR_BLOCK(brightcopy->blkno));
	    bnode_putkey(bparent, boffset, key);
	}

	bnode_free(bleftcopy);
	bnode_free(brightcopy);
	free(bval, M_SLOS);

	if (pickedleft) {
	    if (bright != NULL)
		bnode_free(bright);

	    btree_addelem(btree, bleft);
	} else {
	    if (bleft != NULL)
		bnode_free(bleft);

	    btree_addelem(btree, bright);
	}

	btree_addelem(btree, *bnode);

	/* 
	 * Do not write the parent to disk yet, 
	 * this will be done in bnode_delete.
	 * Forward the in-memory bnode to it.
	 */
	*bnode = bparent;

	return 0;


error:
	/* 
	 * Something went wrong, free all
	 * resources allocated for the borrow. 
	 */
	if (blkleft != 0)
	    btree_blkput(&slos, btree, blkleft);

	if (blkright != 0)
	    btree_blkput(&slos, btree, blkright);

	bnode_free(bleft);
	bnode_free(bright);
	bnode_free(bleftcopy);
	bnode_free(brightcopy);
	free(bval, M_SLOS);

	return error;
}

static int
bnode_merge(struct btree *btree, struct bnode **bnode)
{
	struct bnode *bleft = NULL, *bright = NULL, *bnew = NULL, *bparent = NULL;
	size_t boffset;
	daddr_t blkid = 0;
	uint64_t key;
	int error = 0;

	/* If we're the only node, do nothing. */
	if (btree->root == (*bnode)->blkno && (*bnode)->external != 0)
	    return 0;

	/* 
	 * Allocate on-disk blocks for 
	 * any bnodes we might create.
	 */
	blkid = btree_blkget(&slos, btree);
	if (blkid == 0) {
	    error = ENOSPC;
	    goto error;
	}

	bparent = bnode_parent(*bnode);
	if (bparent == NULL) {
	    error = EIO;
	    goto error;
	}

	/* 
	 * Find our position in the parent node. If nodes need to
	 * be large, this should be replaced with a binary search
	 * routine.
	 */
	boffset = bnode_parentoff(*bnode, bparent);

	KASSERT(boffset != (*bnode)->parent->size + 1, "node is in parent");

	/*
	 * Otherwise, we have siblings. Thankfully, merging two
	 * siblings is _commutative_. Therefore, we use the same
	 * code whether we are merging with our left or our right
	 * sibling. 
	 *
	 * If we are calling this function, borrowing from a sibling has
	 * failed, so that means that the node that will result
	 * from merging with sibling nodes won't be full.
	 *
	 * The parent is obviously an internal node, so the rightmost child
	 * will be at offset size, not (size - 1) as with internal nodes.
	 */


	if (boffset == bparent->size) {
	    bleft = bnode_child(bparent, boffset - 1);
	    if (bleft == NULL) {
		error = EIO;
		goto error;
	    }

	    bright = *bnode;

	    /* Have the offset always point to the left sibling. */
	    boffset -= 1;

	} else {
	    bleft = *bnode;

	    bright = bnode_child(bparent, boffset + 1);
	    if (bright == NULL) {
		error = EIO;
		goto error;
	    }
	}

	bnew = bnode_copy(&slos, blkid, bleft);
	if (bnew == NULL) {
	    error = EIO;
	    goto error;
	}
	bnew->parent.offset = bparent->blkno;


	/* Save the key for later. */
	bnode_getkey(bparent, boffset, &key);

	/* Remove left key/child pair between the nodes in the parent. */
	bnode_shiftl(bparent, boffset, 1);

	/* However, we keep the _left_ sibling, so replace the right one. */
	bnode_putptr(bparent, boffset, DISKPTR_BLOCK(bnew->blkno)); 

	if (bnew->external == BNODE_EXTERNAL) {
	    /* Bring in the right node's keys into the left one. */
	    bnode_copydata(bnew, bright, bnew->size, 0, bright->size);

	    bnew->size += bright->size;
	} else {
	    /*
	     * Get an extra key to separate the rightmost child of the
	     * left node and the leftmost child of the right node. We 
	     * use the key that separates the nodes in the parent.
	     * (It's an interesting question whether it matters for performance
	     * - can we have such a selection of keys that splits are minimal?)
	     */
	    bnode_putkey(bnew, bnew->size, key);

	    /* Bring in the right node's existing keys into the left one. */
	    bnode_copydata(bnew, bright, bnew->size + 1, 0, bright->size + 1);

	    bnew->size += bright->size + 1;

	}

	/* write the new nodes. */
	error = bnode_write(&slos, bnew);
	if (error != 0)
	    goto error;


	/* 
	 * If the merging caused the parent
	 * to have 0 keys, then the parent is
	 * the root, so the root is empty
	 * and bnew will replace it.
	 */
	if (bparent->size == 0) {
	    bnew->parent.offset = bnew->blkno;
	    *bnode = bnew;
	    btree_addelem(btree, bparent);
	} else {
	    /* No root merge, write out the parent. */
	    *bnode = bparent;
	    bnode_free(bnew);
	}

	btree_addelem(btree, bleft);
	btree_addelem(btree, bright);

	return 0;

error:
	/* 
	 * Something went wrong, resources 
	 * allocated for the merge.
	 */

	if (blkid != 0)
	    btree_blkput(&slos, btree, blkid);

	bnode_free(bnew);
	bnode_free(bparent);

	/* Only free the bnode's sibling. */
	if (bleft == *bnode)
	    bnode_free(bright);
	else
	    bnode_free(bleft);

	return error;
}

int
btree_delete(struct btree *btree, uint64_t key)
{
	struct bnode *bnode;
	uint64_t bkey;
	int error, i; 

	btree_clearflags(btree);

	/* Get the only node that could contain the key. */
	bnode = btree_candidate(btree, key);
	for (i = 0; i < bnode->size; i++) {
	    bnode_getkey(bnode,i ,&bkey);
	    if (key < bkey) {
		bnode_free(bnode);
		return EINVAL;
	    }

	    if (key == bkey)
		break;
	}

	/* Key not present. */
	if (i == bnode->size) {
	    bnode_free(bnode);
	    return EINVAL;
	}

	/* Found it, scoot over all keys and values to make room. */
	bnode_shiftl(bnode, i, 1);

	while (((bnode->external == 0) && (2 * (bnode->size + 1) < bnode->bsize))
		|| (bnode->external != 0 && (2 * bnode->size < bnode->bsize))) {
	    /* 
	     * We can't do anything if the root is too sparse. 
	     * When that happens, either the root is an external node
	     * and we can't do anything, or it is internal and so
	     * gets removed during the merging of its children.
	     * In any case, we can't do anything here.
	     */
	    if (bnode->parent.offset == bnode->blkno)
		break;

	    /* First try to borrow from the next node. */
	    error = bnode_borrow(btree, &bnode);
	    if (error == EINVAL) {

		error = bnode_merge(btree, &bnode);
		if (error != 0)
		    goto error;

		btree->op_caused_merge = 1;
		btree->merges += 1;

	    } else if (error != 0) {
		/* We had an error, clean up. */
		goto error;

	    } else {
		btree->op_caused_borrow = 1;
		btree->borrows += 1;
		break;
	    }
	}

	/* 
	 * write the bnode, whether this 
	 * is the original external bnode
	 * or one of its ancestors which
	 * had a key deleted without needing
	 * merging.
	 */
	bnode_write(&slos, bnode);


	/* 
	 * If our parent is the root and it has only one child, then
	 * we don't need it anymore. Replace it with the left node.
	 */
	if (bnode->parent.offset == bnode->blkno && btree->root != bnode->blkno) {
	    btree->root = bnode->blkno;
	    btree->depth -= 1;
	}

	bnode_free(bnode);

	btree->last_op = OPDELETE;
	btree->deletes += 1;
	btree->size -= 1;

	return 0;

error:
	bnode_free(bnode);
	btree_keepelem(btree);

	return error;
}

#ifdef SLOS_TESTS

/* Constants for the testing function. */

#define KEYSPACE    50000	/* Number of keys used */
#define KEYINCR	    100		/* Size of key range */
#define ITERATIONS  100000	/* Number of iterations */
#define CHECKPER    10000	/* Iterations between full tree checks */
#define VALSIZE	    (sizeof(uint64_t)) /* Size of the btree's values */
#define POISON	    'b'		/* Poison byte for the values. */

#define	PINSERT	    40		/* Weight of insert operation */
#define PDELETE	    60		/* Weight of delete operation */
#define PSEARCH	    20		/* Weight of search operation */
#define POVERWRITE  20		/* Weight of overwrite operation */
#define PKEYLIMIT   20 		/* Weight of keymin/keymax operations */

/*
 * Test the whole btree for ordering constraints.
 * This is much more efficient than testing each bnode
 * separately, because we do not need to traverse every
 * subtree to find its minimum value. The traversal
 * is inorder.
 */
static int
btree_test_invariants(struct btree *btree)
{
	struct bnode *bnode, *bparent, *bchild;
	uint64_t lastkey;
	uint64_t firstloop;
	int boffset, downward;
	uint64_t bkey;
	int error;

	error = bnode_read(&slos, btree->root, &bnode);
	if (error != 0)
	    return error;

	/* Reach the leftmost bnode. */
	while (bnode->external == BNODE_INTERNAL) {
	    bparent = bnode;
	    
	    bnode = bnode_child(bnode, 0);
	    if (bnode == NULL) {
		printf("ERROR: NULL child found on offset 0\n");
		bnode_print(bparent);
		bnode_free(bparent);
		return EINVAL;
	    }

	    if (bnode->parent.offset != bparent->blkno) {
		bnode->parent.offset = bparent->blkno;
		bnode_write(&slos, bnode);
	    }

	    bnode_free(bparent);
	}
	downward = 0;
	boffset = 0;

	/* 
	 * We consider the "last key seen" to be 
	 * the first key of the leftmost node.
	 * The last key seen can be internal
	 * _or_ external. Unfortunately, since
	 * keys are unsigned and include 0, there
	 * is no way to set lastkey to be _less_
	 * than all possible values of the first key
	 * in the btree. That's why we use the firstloop
	 * variable below.
	 */
	bnode_getkey(bnode, 0, &lastkey);

	for (firstloop = 1;; firstloop = 0) {
	    /* 
	     * If this is an external node or we are
	     * traversing it for the first time, do
	     * all checks. 
	     */
	    if (bnode->external != 0 || boffset == 0) {
		bnode_getkey(bnode, 0, &bkey);
		if (firstloop == 0 && lastkey >= bkey) {
		    printf("ERROR: Ordering violation for keys %lu, %lu\n", 
			    lastkey, bkey);
		    bnode_print(bnode);
		    return EINVAL;
		}

		/* Test the bnode with regards to its size */
		if (bnode_isordered(bnode) != BNODE_OK) {
		    printf("ERROR: keys are in the wrong order\n");
		    bnode_print(bnode);
		    return EINVAL;
		}

		if (bnode_issized(bnode) != BNODE_OK) {
		    printf("ERROR: bnode has improper size\n");
		    bnode_print(bnode);
		    return EINVAL;
		}
	    }

	    /* 
	     * If this is the root we need to check whether
	     * we're done checking.
	     */
	    if (bnode->parent.offset == bnode->blkno) {
		/* If the tree is only one node we're done. */
		if (bnode->external != 0)
		    break;

		/* 
		 * If we returned from the rightmost subtree 
		 * of the root then we're done.
		 */
		if (boffset == bnode->size)
		    break;
	    }

	    /* 
	     * If we're going downwards, that means that we're accessing
	     * a new subtree. Go to its leftmost node. Since we arrived
	     * from the child with offset boffset, the new subtree is
	     * accessible at boffset + 1;
	     */
	    if (downward == 1) {
		bparent = bnode;
		
		bnode = bnode_child(bnode, boffset + 1);
		if (bnode == NULL) {

		    printf("ERROR: NULL child found on offset %d\n", boffset + 1);
		    bnode_print(bparent);
		    bnode_free(bparent);
		    return EINVAL;
		}

		if (bnode->parent.offset != bparent->blkno) {
		    bnode->parent.offset = bparent->blkno;
		    bnode_write(&slos, bnode);
		}

		bnode_free(bparent);
		
		while (bnode->external == 0) {
		    bparent = bnode;
		    bnode = bnode_child(bnode, 0);

		    if (bnode->parent.offset != bparent->blkno) {
			bnode->parent.offset = bparent->blkno;
			bnode_write(&slos, bnode);
		    }

		    bnode_free(bparent);
		}

		downward = 0;
	    } else {
		/* Slide up the right edge of the subtree. */
		do {
		    /* If we're at the root we can't go further up. */
		    if (bnode->blkno == bnode->parent.offset)
			break;
		    
		    bchild = bnode;
		    bnode = bnode_parent(bnode);

		    boffset = bnode_parentoff(bchild, bnode);

		    bnode_free(bchild);
		
		} while (boffset == bnode->size);

		downward = 1;
	    }
	}

	bnode_free(bnode);

	return 0;
}


/*
 * Test an individual node with regards to its size.
 */
static int
bnode_test_size(struct bnode *bnode)
{
	/* 
	* All non-root nodes have to have 
	* between ceil(BSIZE/2) and BSIZE children.
	* Root nodes have to have between 2 and BSIZE children.
	*/

	if ((bnode->parent.offset != bnode->blkno) && (bnode->external != 0) && (2 * bnode->size < bnode->bsize)) {
	    printf("ERROR: external bnode size %d too low for bnode->bsize %lu\n", bnode->size, bnode->bsize);
	    return EINVAL;
	} else if ((bnode->parent.offset != bnode->blkno) && (bnode->external == 0) && (2 * (bnode->size + 1) < bnode->bsize)) {
	    printf("ERROR: internal bnode size %d too low for bnode->bsize %lu\n", bnode->size, bnode->bsize);
	    return EINVAL;
	} else if ((bnode->parent.offset == bnode->blkno) && (bnode->external == 0) && (bnode->size < 1)) {
	    printf("ERROR: root bnode size %d too low for bnode->bsize %lu\n", bnode->size, bnode->bsize);
	    return EINVAL;
	} else if ((bnode->external != 0) && (bnode->size > bnode->bsize)) {
	    printf("ERROR: external bnode size %d too high for bnode->bsize %lu\n", bnode->size, bnode->bsize);
	    return EINVAL;
	} else if ((bnode->external == 0) && (bnode->size > (bnode->bsize -  1))) {
	    printf("ERROR: internal bnode size %d too high for bnode->bsize %lu\n", bnode->size, bnode->bsize);
	    return EINVAL;
	}

	return 0;
}

/*
 * Test the ordered invariant of the btree.
 * Since we cannot traverse it in 
 */
static int
bnode_test_order(struct btree *btree, struct bnode *bnode)
{
	uint64_t childkey, parentkey;
	struct bnode *bparent;
	int boffset;

	bparent = bnode_parent(bnode);

	/*
	 * All nodes need to have their keys ordered in stricty
	 * ascending order. The "right" key in the parent
	 * also needs to be larger than all keys in the children,
	 * and the "left" key needs to be smaller.
	 */
	/* Test the bnode with regards to its size */
	if (bnode_isordered(bnode) != BNODE_OK) {
	    printf("ERROR: keys are in the wrong order\n");
	    bnode_print(bnode);
	    return EINVAL;
	}

	/* Only do the parent check for nonroot nodes. */
	if (bnode->parent.offset != bnode->blkno) {
	    boffset = bnode_parentoff(bnode, bparent);

	    if (boffset == bparent->size + 1) {
		printf("ERROR: node not found in its parent\n");
		bnode_print(bparent);
		return EINVAL;
	    }

	    /* Check right key. */
	    if (boffset < bparent->size) {
		bnode_getkey(bparent, boffset, &parentkey);
		bnode_getkey(bnode, bnode->size - 1, &childkey);
		if (childkey > parentkey) {
		    printf("ERROR: right parent key smaller than largest child key\n");
		    bnode_print(bparent);
		    return EINVAL;
		}
	    }

	    /* Check left key. */
	    if (boffset > 0) {
		bnode_getkey(bparent, boffset - 1, &parentkey);
		bnode_getkey(bnode, 0, &childkey);
		if (childkey <= parentkey) {
		    printf("ERROR: left parent key larger than / equal to smallest child key\n");
		    bnode_print(bparent);
		    return EINVAL;
		}
	    }

	}

	bnode_free(bparent);

	return 0;
}

/* 
 * Test a bnode with regards to 
 * both ordering and size constraints.
 */
static int
bnode_test(struct btree *btree, struct bnode *bnode)
{
    if (bnode_test_size(bnode) != 0)
	return EINVAL;

    if (bnode_test_order(btree, bnode) != 0)
	return EINVAL;

    return 0;
}


/* 
 * Test a whole btree. The numbers, is_there, was_there arrays are the 
 * set of possible keys in the btree, an array that denotes whether the 
 * key should currently be in the btree, and an array that denotes whether
 * the key was ever in the btree.
 */
static int
btree_test(struct btree *btree, uint64_t *keys, int *is_there, int *was_there, void **values)
{
	int op_caused_merge, op_caused_split, op_caused_borrow;
	int last_op, last_key;
	int ret;
	int i;	
	void *value;

	value = malloc(VALSIZE, M_SLOS, M_WAITOK);
	
	ret = btree_test_invariants(btree);
	if (ret != 0) {
	    printf("ERROR: tree traversal failed\n");
	    free(value, M_SLOS);
	    return EINVAL;
	}
	/*
	 * Check if all existing keys are accessible and all 
	 * nonexistent ones are absent.
	 */
	for (i = 0; i < KEYSPACE; i++) {
	    /* 
	     * The diagnostic fields of the btree will be
	     * overwritten by the search, save them.
	     */
	    op_caused_split = btree->op_caused_split;
	    op_caused_borrow = btree->op_caused_borrow;
	    op_caused_merge = btree->op_caused_merge;
	    last_key = btree->last_key;
	    last_op = btree->last_op;

	    ret = btree_search(btree, keys[i], value);
	    /* These are not real successful searches, so do not log them. */
	    if (ret == 0)
		btree->searches -= 1;

	    /* Restore the diagnostic fields. */
	    btree->op_caused_split = op_caused_split;
	    btree->op_caused_borrow = op_caused_borrow;
	    btree->op_caused_merge = op_caused_merge;

	    btree->last_key = last_key;
	    btree->last_op = last_op;

	    if ((is_there[i] != 0) && (ret != 0)) {
		printf("ERROR: existing key %lu not found\n", keys[i]);
		free(value, M_SLOS);
		return EINVAL;
	    }
	    
	    if ((is_there[i] == 0) && (ret == 0)) {
		printf("ERROR: nonexistent key %lu found. Did it ever exist: %s\n", keys[i], was_there[i] != 0 ? "yes" : "no");
		free(value, M_SLOS);
		return EINVAL;
	    }

	    if (ret == 0 && (memcmp(values[i], value, VALSIZE) != 0)) {
		printf("Error: expected value %.*s and actual value %.*s not equal\n", 
			(int) VALSIZE, (char *) values[i], (int) VALSIZE, (char *) value);
	    
		free(value, M_SLOS);
		return EINVAL;
	    }
	    
	}

	free(value, M_SLOS);
	return 0; 
}

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
slos_test_btree(void)
{

	struct btree *btree;
	int error, i, j;
	uint64_t *keys;
	int *is_there, *was_there;
	int operation, index, key_present;
	uint64_t key, limkey;
	void *value, *oldval;
	uint64_t minkey, maxkey;
	struct bnode *bnode;
	uint64_t blkno;
	struct slos_diskptr diskptr;
	void **values;

	/* Create a temporary btree in the data region. */
	diskptr = slos_bootalloc(slos.slos_bootalloc);
	blkno = diskptr.offset;
	if (blkno == 0)
	    return ENOSPC;

	/* 
	 * Our values are of arbitrary size, so create 
	 * a random legible string of characters for each 
	 * one.
	 */
	value = malloc(VALSIZE, M_SLOS, M_WAITOK);

	bnode = bnode_alloc(&slos, blkno, VALSIZE, BNODE_EXTERNAL);
	error = bnode_write(&slos, bnode);
	if (error != 0) {
	    slos_bootfree(slos.slos_bootalloc, DISKPTR_BLOCK(blkno));
	    return error;
	}

	/* Free the bnode, we'll reread it from disk. */
	free(bnode, M_SLOS);

	/* Create a btree with the blcok we created as a root. */
	btree = btree_init(&slos, blkno, ALLOCBOOT);

	/* 
	 * One of the arrays is the actual numbers, the other 
	 * checks whether the number is there, the third checks
	 * whether the number was ever in the btree. Since the numbers
	 * generated increase strictly monotonically, we don't 
	 * need to worry about duplicates.
	 */
	keys = malloc(sizeof(*keys) * KEYSPACE, M_SLOS, M_WAITOK);
	is_there = malloc(sizeof(*is_there) * KEYSPACE, M_SLOS, M_WAITOK | M_ZERO);
	was_there = malloc(sizeof(*was_there) * KEYSPACE, M_SLOS, M_WAITOK | M_ZERO);

	printf("Starting test\n");

	/* 
	 * Begin from 1, because we'd like to keep 0 for invalid keys 
	 * during tests (while testing the upper bound, key - 1 is 
	 * used to denote an invalid upper bound. The dual of this
	 * for an upper bound on the values of the keys is to disallow
	 * the use of the value UINT64_MAX, but, provided KEYINCR * KEYSPACE
	 * is small relative to that value, there is no possibility of it arising.
	 */
	keys[0] = 1;
	for (i = 1; i < KEYSPACE; i++)
	    keys[i] = keys[i - 1] + 1 + (random() % KEYINCR);

	oldval = malloc(VALSIZE, M_SLOS, M_WAITOK);
	values = malloc(sizeof(*values) * KEYSPACE, M_SLOS, M_WAITOK);
	for (i = 0; i < KEYSPACE; i++) {
	    values[i] = malloc(VALSIZE, M_SLOS, M_WAITOK);
	    for (j = 0; j < VALSIZE - 1; j++)
		((char *) values[i])[j] = 'a' + (random() % ('z' - 'a'));
	    ((char *) values[i])[VALSIZE - 1] = '\0';
	}

	for (i = 0; i < ITERATIONS; i++) {
	    operation = random() % (PINSERT + PDELETE + PSEARCH + POVERWRITE + PKEYLIMIT);
	    index = random() % (KEYSPACE - 1);

	    key = keys[index];
	    key_present = is_there[index];

	    if (operation < PINSERT) {

		/* 
		 * Value is irrelevant, so have it 
		 * be the same as the key.
		 */
		error = btree_insert(btree, key, values[index]);
		if (error != 0 && !key_present) {
		    printf("ERROR: Insertion of nonduplicate failed\n");
		    goto out;
		} else if (error == 0 && key_present) {
		    printf("ERROR: Insertion of duplicate succeeded\n");
		    error = EINVAL;
		    goto out;
		}

		is_there[index] = 1;
		was_there[index] = 1;
	    } else if (operation < PINSERT + PDELETE) {

		error = btree_delete(btree, key);
		if (error != 0 && key_present) {
		    printf("ERROR: Deletion of existing key failed\n");
		    goto out;
		} else if (error == 0 && !key_present) {
		    printf("ERROR: Deletion of nonexistent key succeeded\n");
		    error = EINVAL;
		    goto out;
		}

		is_there[index] = 0;
	    } else if (operation < PINSERT + PDELETE + PSEARCH) {

		error = btree_search(btree, key, value);
		if (error != 0 && key_present) {
		    printf("ERROR: Search of existing key failed\n");
		    goto out;
		} else if (error == 0 && !key_present) {
		    printf("ERROR: Search of nonexistent key succeeded\n");
		    error = EINVAL;
		    goto out;
		}

		if (error == 0 && (memcmp(value, values[index], VALSIZE) != 0)) {
		    printf("ERROR: Value %.*s not equal to expected %.*s\n", 
			    (int) VALSIZE, (char *) value, (int) VALSIZE, (char *) values[index]);
		    error = EINVAL;
		    goto out;
		    
		}

	    } else if (operation < PINSERT + PDELETE + PSEARCH + POVERWRITE) {
		/* Give the oldval variable a value that is always invalid */
		memset(oldval, POISON, VALSIZE);

		error = btree_overwrite(btree, key, values[index], oldval);
		/* Overwrites should always succeed if oldval != NULL. */
		if (error != 0) {
		    printf("ERROR: Overwrite of key %lu failed\n", key);
		    error = EINVAL;
		    goto out;
		}

		/* Since key = val always, we have to have value == oldval on success. */
		if (key_present && (memcmp(oldval, values[index], VALSIZE) != 0)) {
		    printf("ERROR: Value %.*s not equal to old value %.*s\n", 
			    (int) VALSIZE, values[index], (int) VALSIZE, oldval);
		    error = EINVAL;
		    goto out;
		} else if (!key_present) {
		    for (j = 0; j < VALSIZE; j++) {
			/* Check the old value to see whether we actually overwrote it. */
			if (((char *) oldval)[j] != POISON) {
			    printf("ERROR: Old value is, %.*s should not have been \
				    written to\n", (int) VALSIZE, oldval);
			    error = EINVAL;
			    goto out;
			}
		    }
		}

		is_there[index] = 1;
		was_there[index] = 1;
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

		error = btree_keymin(btree, &limkey, value);
		if (error != 0 && minkey <= key) {
		    printf("ERROR: Lower bound %lu for key %lu not found\n", minkey, key);
		    error = EINVAL;
		    goto out;
		}

		if (error == 0 && minkey != limkey) {
		    printf("ERROR: Lower bound for key %lu should be %lu, is %lu\n", 
			    key, minkey, limkey);
		    error = EINVAL;
		    goto out;

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

		error = btree_keymax(btree, &limkey, value);
		if (error != 0 && maxkey >= key) {
		    printf("ERROR: Upper bound %lu for key %lu not found\n", maxkey, key);
		    error = EINVAL;
		    goto out;
		}

		if (error == 0 && maxkey != limkey) {
		    printf("ERROR: Upper bound for key %lu should be %lu, is %lu\n", 
			    key, maxkey, limkey);
		    error = EINVAL;
		    goto out;

		}

	    }


	    /* Discard elements of the btree not used anymore. */
	    btree_discardelem(btree);

	    /* 
	     * Every once in a while we do a full tree check. This check makes sure both
	     * invariants of the tree are held (ordering and node capacity).
	     */
	    if (i % CHECKPER == 0) {
		if (btree_test(btree, keys, is_there, was_there, values) != 0) {
		    printf("ERROR: Integrity of btree violated\n");
		    error = EINVAL;
		    goto out;
		}
	    }
	}

	error = 0;
out:
	printf("Iterations: %d\n", i);
	btree_print(btree);

	for (i = 0; i < KEYSPACE; i++)
	    free(values[i], M_SLOS);
	free(values, M_SLOS);

	free(value, M_SLOS);
	free(oldval, M_SLOS);

	free(keys, M_SLOS);
	free(is_there, M_SLOS);
	free(was_there, M_SLOS);
	btree_destroy(btree);
	return error;
}

#endif /* SLOS_TESTS */
