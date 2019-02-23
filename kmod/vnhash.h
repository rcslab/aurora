#ifndef _VNHASH_H_
#define _VNHASH_H_



/*
 * Randomly pick 4Kbuckets
 */
#define HASH_MAX (4 * 1024)

struct dump_filename {
	size_t len;
	char *string;
};

struct dump_vnentry {
	void *vnode;
	struct dump_filename *dump_filename;
	LIST_ENTRY(dump_vnentry) next;
};

LIST_HEAD(vnentry_tailq, dump_vnentry);


extern struct vnentry_tailq *slsnames;

/*
 * The mask defines which bits of a hash are used in the table.
 * Here we use the absolute easiest.
 */
extern u_long vnhashmask;

int setup_vnhash(void);
void cleanup_vnhash(void);

void print_vnbucket(int bucketnum);
void print_vnlist(void);

struct dump_filename *vnhash_find(void *vnode);

#endif /* _VNHASH_H_ */
