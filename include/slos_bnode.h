#ifndef _SLOS_BNODE_H_
#define _SLOS_BNODE_H_

#define BNODE_INTERNAL 0
#define BNODE_EXTERNAL 1

#define SLOS_BMAGIC 0x5105BA1CUL

/* Keys are always 64bit in this implementation. */
#define SLOS_KEYSIZE sizeof(uint64_t)

/*
 * Node for the btree.
 */
struct bnode {
	uint64_t blkno;		    /* The on disk block address of the node */
	uint8_t external;	    /* Is the node a value node? */
	uint64_t bsize;		    /* Length of the bnode's arrays. */
	uint64_t vsize;		    /* Size of the bnode's values. */
	uint16_t size;		    /* Current number of keys */
	struct slos_diskptr parent; /* Block position of the parent node */
	uint64_t magic;		    /* Magic number, used to find corruption */

	uint8_t data[0]; /* Bnode keys and values */
};

#ifdef _KERNEL

int bnode_getkey(struct bnode *bnode, size_t offset, uint64_t *key);
int bnode_getvalue(struct bnode *bnode, size_t offset, void *value);
int bnode_getptr(
    struct bnode *bnode, size_t offset, struct slos_diskptr *diskptr);

int bnode_putkey(struct bnode *bnode, size_t offset, uint64_t key);
int bnode_putvalue(struct bnode *bnode, size_t offset, void *value);
int bnode_putptr(
    struct bnode *bnode, size_t offset, struct slos_diskptr diskptr);

int bnode_read(struct slos *slos, daddr_t blkno, struct bnode **bnodep);
int bnode_write(struct slos *slos, struct bnode *bnode);

int bnode_isordered(struct bnode *bnode);
int bnode_issized(struct bnode *bnode);
int bnode_parentoff(struct bnode *bnode, struct bnode *bparent);

void bnode_print(struct bnode *bnode);
int bnode_copydata(struct bnode *dst, struct bnode *src, size_t dstoff,
    size_t srcoff, size_t len);
void bnode_shiftl(struct bnode *bnode, size_t offset, size_t shift);
void bnode_shiftr(struct bnode *bnode, size_t offset, size_t shift);

struct bnode *bnode_alloc(
    struct slos *slos, uint64_t blkno, uint64_t vsize, int external);
struct bnode *bnode_copy(
    struct slos *slos, uint64_t blkno, struct bnode *bnode);
void bnode_free(struct bnode *bnode);

struct bnode *bnode_parent(struct slos *slos, struct bnode *bnode);
struct bnode *bnode_child(
    struct slos *slos, struct bnode *bnode, size_t offset);

int bnode_search(struct bnode *bnode, uint64_t key, void *value);

/* Possible statuses when checking for malformed bnodes. */
#define BNODE_OK 0
#define BNODE_OOB 1
#define BNODE_OOO 2
#define BNODE_SZERR 3

int bnode_inbounds(struct bnode *bnode, size_t offset);

#endif /* _KERNEL */

#endif /* _SLOS_BNODE_H_ */
