#include <assert.h>
#include <slos.h>
#include <slos_inode.h>
#include <unistd.h>

#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>

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
		printf("| C 0x%lx |", *p);
		for (int i = 0; i < NODE_SIZE(node); i++) {
			p = (bnode_ptr *)fnode_getval(node, i + 1);
			uint64_t __unused *t = (uint64_t *)fnode_getkey(
			    node, i);
			printf("| K %lu|| C 0x%lx |", *t, *p);
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
fnodeSetup(Snapshot *snap, struct fnode *node, struct fbtree *tree, char *buf,
    long ptr)
{
	node->fn_dnode = (struct dnode *)buf;
	node->fn_location = ptr;
	node->fn_bsize = snap->super.sb_bsize;
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
	assert(((char *)node->fn_values + (NODE_MAX(node) * NODE_VS(node))) <=
	    (char *)buf + node->fn_bsize);
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

	auto n = next.follow(key);
	return n;
}

template <typename K, typename V>
K
BtreeIter<K, V>::key()
{
	return *(K *)fnode_getkey(&node.node, at);
}

template <typename K, typename V>
V
BtreeIter<K, V>::val()
{
	return *(V *)fnode_getval(&node.node, at);
}

template <typename K, typename V>
int
BtreeIter<K, V>::valid()
{
	if (node.error) {
		printf("Corrupt Btree Node found at 0x%lx\n",
		    node.node.fn_location);
		return ITER_CORRUPT;
	}
	if (at == -1) {
		return ITER_END;
	}

	return ITER_GOOD;
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

	auto v = (*(V *)fnode_getval(&node, index));
	if (v.offset == blknum) {
		auto n = BtreeNode<K, V>(btree, snap, node.fn_location);
		return BtreeIter<K, V> { n, index };
	}

	/* Prepare to traverse the next node. */
	bnode_ptr ptr = *(bnode_ptr *)fnode_getval(&node, index);
	auto next = BtreeNode<K, V>(btree, snap, ptr);
	return next.follow_to(key, blknum);
}

template <typename K, typename V>
BtreeIter<K, V>
BtreeNode<K, V>::parent()
{
	auto root = btree->getRoot();
	auto keyt = *(K *)fnode_getkey(&node, 0);
	return root.follow_to(keyt, node.fn_location);
}

template <typename K, typename V>
BtreeIter<K, V>
BtreeIter<K, V>::next()
{
	V val;
	if (valid() != 0) {

		return BtreeIter<K, V> {};
	}

	if ((at + 1) == NODE_SIZE(&node.node)) {
		auto r = node.btree->getRoot();
		if (r.node.fn_location != node.node.fn_location) {
			auto parentIter = node.parent();
			while (parentIter.val().offset != node.blknum) {
				parentIter = parentIter.next();
			}
			parentIter = parentIter.next();
			if (parentIter.valid() != 0) {
				return BtreeIter<K, V> {};
			}

			bnode_ptr ptr = *(bnode_ptr *)fnode_getval(
			    &parentIter.node.node, parentIter.at);
			auto n = BtreeNode<K, V>(
			    node.btree, node.snap, static_cast<long>(ptr));
			BtreeIter<K, V> iter = BtreeIter<K, V> { n, 0 };

			return iter;
		}
		return BtreeIter<K, V> {};
	}

	BtreeIter<K, V> iter = BtreeIter(this);
	iter.at++;

	return iter;
}

template <typename K, typename V>
BtreeNode<K, V>::BtreeNode(BtreeNode<K, V> *node)
{
	this->btree = node->btree;
	this->tree = node->tree;
	this->blknum = node->blknum;
	this->snap = node->snap;
	this->error = node->error;
	init();
}

template <typename K, typename V>
BtreeNode<K, V>::BtreeNode(BtreeNode<K, V> const &node)
{
	this->btree = node.btree;
	this->tree = node.tree;
	this->blknum = node.blknum;
	this->snap = node.snap;
	this->error = node.error;
	init();
}

template <typename K, typename V>
BtreeNode<K, V>::BtreeNode(BtreeNode<K, V> &node)
{
	this->btree = node.btree;
	this->tree = node.tree;
	this->blknum = node.blknum;
	this->snap = node.snap;
	this->error = node.error;
	init();
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

	return BtreeIter<K, V>(*this, i);
}

template <typename K, typename V>
int
BtreeNode<K, V>::failed()
{
	return error;
}

template <typename K, typename V>
int
BtreeNode<K, V>::init()
{
	size_t blksize = snap->super.sb_bsize;
	int readin = pread(snap->dev, data, blksize, blknum * blksize);
	if (readin != blksize) {
		std::cout << "Error reading in btree node" << std::endl;
		error = 1;
		return (-1);
	}

	tree.bt_keysize = sizeof(K);
	tree.bt_valsize = sizeof(V);

	fnodeSetup(snap, &node, &tree, data, blknum);
	if (node.fn_dnode->dn_magic != DN_MAGIC) {
		printf("BTree Corrupt at: 0x%lx\n", node.fn_location);
		error = 1;
		return (-1);
	}

	error = 0;
	return (0);
}

template <typename K, typename V> BtreeNode<K, V>::~BtreeNode<K, V>()
{
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
	return BtreeNode<K, V>(this, snap, blknum);
}

template <typename K, typename V>
std::string
BtreeNode<K, V>::toString()
{
	std::stringstream ss;
	ss << std::setbase(16);
	bnode_ptr v = *(bnode_ptr *)fnode_getval(&node, 0);
	ss << "| C 0x" << v << "|";
	if (type() == BT_INTERNAL) {
		for (int i = 0; i < NODE_SIZE(&node); i++) {
			K k = *(K *)fnode_getkey(&node, i);
			bnode_ptr v = *(bnode_ptr *)fnode_getval(&node, i + 1);
			ss << "| K 0x" << k << "|| C 0x" << v << "|";
		}
	} else {
		for (int i = 0; i < NODE_SIZE(&node); i++) {
			K k = *(K *)fnode_getkey(&node, i);
			ss << "| K 0x" << k << "|";
		}
	}

	return ss.str();
}

template <typename K, typename V>
std::string
Btree<K, V>::toString()
{
	std::queue<std::pair<BtreeNode<K, V>, int>> bfs;
	auto root = getRoot();
	bfs.push(std::make_pair(root, 0));
	int cur_layer = 0;
	// Keeping track of the height so we can have empty lines seperating
	// each layer of the tree
	std::stringstream ss;
	ss << std::setbase(16);
	while (!bfs.empty()) {
		auto p = bfs.front();
		auto layer = p.second;
		auto node = p.first;
		bfs.pop();

		if (cur_layer < layer) {
			ss << "EndLayer";
			cur_layer += 1;
			ss << std::endl;
		}
		if (node.type() == BT_INTERNAL) {
			for (int i = 0; i < NODE_SIZE(&node.node); i++) {
				bnode_ptr p = *(bnode_ptr *)fnode_getval(
				    &node.node, i);
				auto n = BtreeNode<K, V>(this, snap, p);
				bfs.push(std::make_pair(n, cur_layer + 1));
			}
		}

		ss << node.toString() << ">>>" << std::endl;
	}

	return ss.str();
}

template <typename K, typename V>
int
BtreeNode<K, V>::verify()
{
	if (error) {
		printf("Corrupt Node: 0x%lx?\n", node.fn_location);
		return (-1);
	}

	if (type() == BT_INTERNAL) {
		for (int i = 0; i < NODE_SIZE(&node); i++) {
			bnode_ptr p = *(bnode_ptr *)fnode_getval(&node, i);
			auto n = BtreeNode<K, V>(btree, snap, p);
			if (n.verify()) {
				return (-1);
			}
		}
	}

	return (0);
}
template <typename K, typename V>
int
Btree<K, V>::verify()
{
	auto root = getRoot();
	return root.verify();
}

template <typename K, typename V>
BtreeIter<K, V>
Btree<K, V>::keymax(K key)
{
	auto root = getRoot();
	if (root.size() == 0) {
		return BtreeIter<K, V> {};
	}

	auto node = root.follow(key);
	auto iter = node.keymax(key);

	return iter;
}

template class Btree<unsigned long, slos_diskptr>;
template class BtreeIter<unsigned long, slos_diskptr>;
template class BtreeNode<unsigned long, slos_diskptr>;
