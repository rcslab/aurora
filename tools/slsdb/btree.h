#ifndef __SLSBTREE_H__
#define __SLSBTREE_H__

#define ITER_GOOD (0)
#define ITER_CORRUPT (1)
#define ITER_END (2)

class Snapshot;

template <typename K, typename V> class BtreeNode;

template <typename K, typename V> class Btree;

template <typename K, typename V> class BtreeIter {
    public:
	BtreeIter<K, V>(BtreeIter<K, V> *n)
	    : node(n->node)
	    , at(n->at) {};
	BtreeIter<K, V>()
	    : at(-1)
	    , node() {};
	BtreeIter<K, V>(BtreeNode<K, V> &node, int at)
	    : node(node)
	    , at(at) {};

	K key();
	V val();

	BtreeIter<K, V> next();
	int valid();
	BtreeNode<K, V> node;
	int at;
};

template <typename K, typename V> class BtreeNode {
    public:
	BtreeNode<K, V>(Btree<K, V> *btree, Snapshot *sb, long blknum)
	    : btree(btree)
	    , snap(sb)
	    , blknum(blknum)
	{
		init();
	};
	BtreeNode<K, V>(BtreeNode<K, V> *node);
	BtreeNode<K, V>(BtreeNode<K, V> &node);
	BtreeNode<K, V>(BtreeNode<K, V> const &node);
	BtreeNode<K, V>() = default;
	~BtreeNode<K, V>();

	int init();
	int failed();
	int verify();
	int type() { return NODE_TYPE(&node); }
	int size() { return node.fn_dnode->dn_numkeys; }

	BtreeIter<K, V> follow_to(K key, long blknum);
	BtreeNode<K, V> follow(K key);
	BtreeIter<K, V> parent();
	void print();
	std::string toString();

	BtreeIter<K, V> keymax(K key);

	Btree<K, V> *btree;
	struct fbtree tree;
	struct fnode node;
	int error = 0;
	char data[4096];
	Snapshot *snap;
	long blknum;
};

template <typename K, typename V> class Btree {
    public:
	Btree() = default;
	Btree<K, V>(Snapshot *sb, long blknum)
	    : snap(sb)
	    , blknum(blknum) {};

	BtreeIter<K, V> keymax(K key);
	BtreeNode<K, V> getRoot();
	int verify();
	std::string toString();

	Snapshot *snap;
	struct fbtree tree;
	long blknum;
};
#endif
