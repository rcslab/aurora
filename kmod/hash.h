#ifndef _HASH_H_
#define _HASH_H_



/*
 * Randomly pick 4Kbuckets
 */
#define HASH_MAX (4 * 1024)

struct dump_page {
	vm_ooffset_t vaddr;
	void *data;
	LIST_ENTRY(dump_page) next;
};

LIST_HEAD(page_tailq, dump_page);


extern struct page_tailq *slspages;

/*
 * The mask defines which bits of a hash are used in the table.
 * Here we use the absolute easiest.
 */
extern u_long hashmask;

int setup_hashtable(void);
void cleanup_hashtable(void);

void print_bucket(int bucketnum);
void print_list(void);

#endif /* _HASH_H_ */
