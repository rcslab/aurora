#ifndef _SRDX_H_
#define _SRDX_H_

#define STREE_INVAL \
	((diskblk_t) { 0xbabababababababaULL, 0xcdcdcdcdcdcdcdcdULL })
#define STREE_VALSIZE (sizeof(diskblk_t))
#define STREE_VALVALID(value) ((value).offset != STREE_INVAL.offset)

#define STREE_DEPTH (5)
#define SRDXCAP (BLKSIZE / STREE_VALSIZE)
#define SRDXMASK (SRDXCAP - 1)
#define SRDXVAL(srdx, i) (((diskblk_t *)(srdx))[(i)])
#define KEYMAX ((SRDXCAP * SRDXCAP * SRDXCAP * SRDXCAP * SRDXCAP))

typedef void *rdxnode;

struct stree_iter {
	rdxnode siter_nodes[STREE_DEPTH];
	uint64_t siter_key;
	diskblk_t siter_value;
};

int siter_start(int fd, uint64_t root, uint64_t key, struct stree_iter *siter);
int siter_iter(int fd, struct stree_iter *siter);
void siter_end(struct stree_iter *siter);

#endif /* _SRDX_H_ */
