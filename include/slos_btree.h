#ifndef _SLOS_BTREE_H_
#define _SLOS_BTREE_H_

/* Opcodes for the btree operations. */
#define OPINSERT 0
#define OPDELETE 1
#define OPSEARCH 2
#define OPOVERWRITE 3
#define OPKEYMIN 4
#define OPKEYMAX 5

/* Types of allocators that can be used. */
#define ALLOCBOOT 0
#define ALLOCMAIN 1

struct belem {
	struct bnode *bnode;	   /* The bnode to be freed at operation end */
	LIST_ENTRY(belem) entries; /* Linked list of bnodes to be freed */
};

LIST_HEAD(btreeq, belem);

#define BTREE_KEY_FOREACH(btree, key, val)                     \
	key = 0;                                               \
	for (int err##btree = btree_keymin(btree, &key, &val); \
	     err##btree == 0;                                  \
	     key = key + 1, err##btree = btree_keymin(btree, &key, &val))

/* An in-memory structure for a btree. */
struct btree {
	uint64_t root;	      /* The root of the tree */
	struct btreeq btreeq; /* Queue of elements to be freed
				 if transaction succeeds */

	uint64_t ptrblk; /* Disk which has a blkptr to the btree. */
	uint64_t ptroff; /* Offset in the above block */
	int alloctype;	 /* Use the boot, or the main allocator? */

	/* Stats and debug info */
	uint8_t last_op;      /* What was the last op done? */
	uint64_t last_key;    /* What was the key for the last op? */
	int op_caused_merge;  /* Did the last op cause a merge? */
	int op_caused_split;  /* Did the last op cause a split? */
	int op_caused_borrow; /* Did the last op cause a borrow? */

	uint64_t inserts;    /* Number of successful inserts */
	uint64_t deletes;    /* Number of successful deletes */
	uint64_t searches;   /* Number of successful searches */
	uint64_t overwrites; /* Number of successful overwrites */
	uint64_t keymins;    /* Number of successful keymins */
	uint64_t keymaxes;   /* Number of successful keymaxes */
	uint64_t merges;     /* Number of merges done */
	uint64_t splits;     /* Number of splits done */
	uint64_t borrows;    /* Number of borrows done */

	uint64_t size;	/* Total number of keys */
	uint64_t depth; /* Depth of the btree */

	struct slos *slos;
};

#ifdef _KERNEL
struct btree *btree_init(struct slos *slos, uint64_t blkno, int alloctype);
void btree_destroy(struct btree *btree);

struct bnode *btree_bnode(struct btree *btree, uint64_t key);
int btree_empty(struct btree *btree, int *is_empty);
int btree_first(struct btree *btree, uint64_t *key, void *value);
int btree_last(struct btree *btree, uint64_t *key, void *value);
int btree_search(struct btree *btree, uint64_t key, void *value);
int btree_insert(struct btree *btree, uint64_t key, void *value);
int btree_delete(struct btree *btree, uint64_t key);
void btree_print(struct btree *btree);

int btree_overwrite(
    struct btree *btree, uint64_t key, void *value, void *pastval);
int btree_keymin(struct btree *btree, uint64_t *key, void *value);
int btree_keymax(struct btree *btree, uint64_t *key, void *value);

void btree_addelem(struct btree *btree, struct bnode *bnode);
void btree_discardelem(struct btree *btree);
void btree_keepelem(struct btree *btree);

int slosalloc_fbtree_init(struct slos *slos, size_t offset, diskptr_t *ptr);
#endif

#endif /* _BTREE_H_ */
