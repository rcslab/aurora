
#include <slos.h>
#include <slos_inode.h>
#include <unistd.h>

#include <btree.h>

#include <iostream>

#include "btree.h"
#include "file.h"
#include "snapshot.h"

void
printBtreeNode(struct fnode *node)
{
	int i;
	if (node == NULL) {
		return;
	}
	printf("Btree Node: %lu\n", node->fn_location);
	printf("(%u)size (%u)type\n", NODE_SIZE(node), NODE_TYPE(node));
	if (NODE_TYPE(node) == BT_INTERNAL) {
		bnode_ptr *p = (bnode_ptr *)fnode_getval(node, 0);
		printf("| C %lu |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = (bnode_ptr *)fnode_getval(node, i + 1);
			uint64_t __unused *t = (uint64_t *)fnode_getkey(
			    node, i);
			printf("| K %lu|| C %lu |", *t, *p);
		}
	} else if (NODE_TYPE(node) == BT_EXTERNAL) {
		for (i = 0; i < NODE_SIZE(node); i++) {
			uint64_t *t = (uint64_t *)fnode_getkey(node, i);
			if (node->fn_tree->bt_valsize == sizeof(diskptr_t)) {
				diskptr_t *v = (diskptr_t *)fnode_getval(
				    node, i);
				printf("| %lu (%u)-> %lu, %lu, %lu |,", *t,
				    node->fn_types[i], v->offset, v->size,
				    v->epoch);
			} else {
				uint64_t *v = (uint64_t *)fnode_getval(node, i);
				printf("| %lu (%u)-> %lu|,", *t,
				    node->fn_types[i], *v);
			}
		}
	}
	printf("\n");
}

void *
fnode_getkey(struct fnode *node, int i)
{
	return (char *)node->fn_keys + (i * node->fn_tree->bt_keysize);
}

void *
fnode_getval(struct fnode *node, int i)
{
	return (char *)node->fn_values + (i * NODE_VS(node));
}

void
fnodeSetup(struct fnode *node, struct fbtree *tree, char *buf, long ptr)
{
	node->fn_dnode = (struct dnode *)buf;
	node->fn_location = ptr;
	node->fn_tree = tree;
	if (NODE_TYPE(node) == BT_INTERNAL) {
		node->fn_types = NULL;
		node->fn_keys = (char *)node->fn_dnode->dn_data;
		node->fn_values = (char *)node->fn_keys +
		    (NODE_MAX(node) * NODE_KS(node));
	} else if (NODE_TYPE(node) == BT_BUCKET) {
		node->fn_types = NULL;
		node->fn_keys = (char *)node->fn_dnode->dn_data;
		node->fn_values = (char *)node->fn_keys + (NODE_KS(node));
	} else {
		node->fn_types = (uint8_t *)node->fn_dnode->dn_data;
		node->fn_keys = (char *)node->fn_types +
		    (sizeof(uint8_t) * NODE_MAX(node));
		node->fn_values = (char *)node->fn_keys +
		    (NODE_MAX(node) * NODE_KS(node));
	}
}

template <typename K, typename V>
BtreeNode<K, V>
BtreeNode<K, V>::follow(K key)
{
	if (NODE_TYPE(&node) != BT_INTERNAL) {
		return *this;
	}

	int index, start, end, mid, compare;
	K keyt, keyn;
	start = 0;
	end = NODE_SIZE(&node) - 1;
	if (NODE_SIZE(&node)) {
		index = -1;
		while (start <= end) {
			mid = (start + end) / 2;
			keyt = *(K *)fnode_getkey(&node, mid);
			if (keyt < key) {
				start = mid + 1;
			} else {
				index = mid;
				end = mid - 1;
			}
		}

		if (index == -1) {
			index = NODE_SIZE(&node) - 1;
		}
	} else {
		index = 0;
	}

	while (index > 0) {
		keyt = *(K *)fnode_getkey(&node, index - 1);
		keyn = *(K *)fnode_getkey(&node, index);
		if (keyt != keyn) {
			break;
		} else {
			index--;
		}
	}

	if ((*(K *)fnode_getkey(&node, index)) <= key) {
		index++;
	}
	/* Prepare to traverse the next node. */
	bnode_ptr ptr = *(bnode_ptr *)fnode_getval(&node, index);
	auto next = BtreeNode<K, V>(btree, snap, ptr);
	next.parentkey = *(K *)fnode_getkey(&node, index);

	return next.follow(key);
}

template <typename K, typename V>
K
BtreeIter<K, V>::key()
{
	return *(K *)fnode_getkey(&node->node, at);
}

template <typename K, typename V>
V
BtreeIter<K, V>::val()
{
	return *(V *)fnode_getval(&node->node, at);
}

template <typename K, typename V>
int
BtreeIter<K, V>::valid()
{
	return (at != -1);
}

template <typename K, typename V>
BtreeIter<K, V>
BtreeNode<K, V>::follow_to(K key, long blknum)
{

	int index, start, end, mid, compare;
	K keyt, keyn;
	start = 0;
	end = NODE_SIZE(&node) - 1;
	if (NODE_SIZE(&node)) {
		index = -1;
		while (start <= end) {
			mid = (start + end) / 2;
			keyt = *(K *)fnode_getkey(&node, mid);
			if (keyt < key) {
				start = mid + 1;
			} else {
				index = mid;
				end = mid - 1;
			}
		}

		if (index == -1) {
			index = NODE_SIZE(&node) - 1;
		}
	} else {
		index = 0;
	}

	while (index > 0) {
		keyt = *(K *)fnode_getkey(&node, index - 1);
		keyn = *(K *)fnode_getkey(&node, index);
		if (keyt != keyn) {
			break;
		} else {
			index--;
		}
	}

	if ((*(K *)fnode_getkey(&node, index)) <= key) {
		index++;
	}
	/* Prepare to traverse the next node. */
	bnode_ptr ptr = *(bnode_ptr *)fnode_getval(&node, index);
	auto next = BtreeNode<K, V>(btree, snap, ptr);
	next.parentkey = *(K *)fnode_getkey(&node, index);

	if (next.blknum == blknum) {
		auto n = std::make_shared<BtreeNode<K, V>>(&next);
		return BtreeIter<K, V> { n, index };
	} else {
		return next.follow_to(key, blknum);
	}
}

template <typename K, typename V>
BtreeIter<K, V>
BtreeNode<K, V>::parent()
{
	if (parentkey == -1) {
		return BtreeIter<K, V> {};
	}

	auto root = btree->getRoot();
	auto parent = root.follow_to(parentkey, blknum);

	return parent;
}

template <typename K, typename V>
BtreeIter<K, V>
BtreeIter<K, V>::next()
{
	V val;
	BtreeIter<K, V> iter;

	if (!valid()) {

		return BtreeIter<K, V> {};
	}

	if ((at + 1) == NODE_SIZE(&node->node)) {
		if (node->parentkey != -1) {
			auto parentIter = node->parent();
			parentIter.next();
			if (!parentIter.valid()) {
				return BtreeIter<K, V> {};
			}

			bnode_ptr ptr = *(bnode_ptr *)fnode_getval(
			    &parentIter.node->node, parentIter.at);
			auto n = std::make_shared<BtreeNode<K, V>>(
			    node->btree, node->snap, static_cast<long>(ptr));
			iter = BtreeIter<K, V> { n, 0 };

			return iter;
		}
		return BtreeIter<K, V> {};
	}

	iter = BtreeIter(this);
	iter.at++;

	return iter;
}

template <typename K, typename V>
BtreeNode<K, V>::BtreeNode(BtreeNode<K, V> *node)
{
	this->data = node->data;
	node->data = nullptr;

	this->btree = node->btree;
	this->node = node->node;
	this->tree = node->tree;
	this->node.fn_tree = &this->tree;
	this->parentkey = node->parentkey;
	this->snap = node->snap;
	this->error = node->error;
}

template <typename K, typename V>
BtreeIter<K, V>
BtreeNode<K, V>::keymax(K key)
{
	K keyt;
	int i = 0;
	for (i = 0; i < NODE_SIZE(&node); i++) {
		keyt = *(K *)fnode_getkey(&node, i);
		if (keyt >= key) {
			break;
		}
	}

	auto node = std::make_shared<BtreeNode<K, V>>(this);
	return BtreeIter<K, V>(node, i);
}

template <typename K, typename V>
int
BtreeNode<K, V>::failed()
{
	if (data == nullptr) {
		return (-1);
	}

	return error;
}

template <typename K, typename V>
int
BtreeNode<K, V>::init()
{
	size_t blksize = snap->super.sb_bsize;
	data = (char *)malloc(snap->super.sb_bsize);
	int readin = pread(snap->dev, data, blksize, blknum * blksize);
	if (readin != blksize) {
		std::cout << "Error reading in btree node" << std::endl;
		error = 1;
		return (-1);
	}

	tree.bt_keysize = sizeof(K);
	tree.bt_valsize = sizeof(V);

	fnodeSetup(&node, &tree, data, blknum);
	error = 0;
	return (0);
}

template <typename K, typename V> BtreeNode<K, V>::~BtreeNode<K, V>()
{
	if (data) {
		free(data);
	}
}

template <typename K, typename V>
void
BtreeNode<K, V>::print()
{
	printBtreeNode(&node);
}

template <typename K, typename V>
BtreeNode<K, V>
Btree<K, V>::getRoot()
{
	auto root = BtreeNode<K, V>(this, snap, blknum);
	root.init();

	return root;
}

template <typename K, typename V>
BtreeIter<K, V>
Btree<K, V>::keymax(K key)
{
	auto root = getRoot();
	if (root.failed()) {
		std::cout << "Failured retrieving root" << std::endl;
		return BtreeIter<K, V> {};
	}

	auto node = root.follow(key);
	if (node.failed()) {
		return BtreeIter<K, V> {};
	}

	auto iter = node.keymax(key);

	return iter;
}

template class Btree<unsigned long, slos_diskptr>;
template class BtreeIter<unsigned long, slos_diskptr>;
template class BtreeNode<unsigned long, slos_diskptr>;
