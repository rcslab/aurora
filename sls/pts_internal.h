#ifndef _PTS_INTERNAL_
#define _PTS_INTERNAL_

#define TTYINQ_QUOTESIZE (TTYINQ_DATASIZE / BMSIZE)
#define BMSIZE 32

struct ttyinq_block {
	struct ttyinq_block *tib_prev;
	struct ttyinq_block *tib_next;
	uint32_t tib_quotes[TTYINQ_QUOTESIZE];
	char tib_data[TTYINQ_DATASIZE];
};

struct ttyoutq_block {
	struct ttyoutq_block *tob_next;
	char tob_data[TTYOUTQ_DATASIZE];
};

#endif /* _PTS_INTERNAL_ */
