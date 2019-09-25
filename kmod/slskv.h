#ifndef	_SLSKV_H_ 
#define	_SLSKV_H_ 

#include <sys/param.h>

#include <machine/param.h>

#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/queue.h>

#include <sys/lock.h>
#include <sys/mutex.h>

#include <vm/uma.h>

/* The number of buckets created. */
#define SLSKV_BUCKETS (1024)

/* 
 * Generic hashtable data structure. Built on top
 * of FreeBSD's existing hashtable functions. 
 */
struct slskv_pair {
	uint64_t		key;	/* A 64bit key */
	uintptr_t		value;	/* Pointer to a value of arbitrary size, 
					   or possibly the value itself. */
	LIST_ENTRY(slskv_pair)	next;	/* Used for chaining in the hashtable */
};

LIST_HEAD(slskv_pairs, slskv_pair);	/* A bucket of the hashtable */

/* 
 * Replace policy for conflicting keys. When we try to add a new key only
 * to find that it already exists in the structure, we can either keep the
 * old value, replace it with the new value, or allow multiple values 
 * for the same key. For the latter case, deleting a key only deletes one 
 * instance of it, and searching similarly returns an arbitrary instance.
 */
enum slskv_policy { SLSKV_REPLACE, SLSKV_NOREPLACE, SLSKV_MULTIPLES };

/* Values can either be numbers (64 bit by default), or pointers. */
enum slskv_valtype { SLSKV_VALNUM, SLSKV_VALPTR };

/* The main key-value table. */
struct slskv_table {
	struct mtx_padalign mtx[SLSKV_BUCKETS];	/* Per-bucket locking */
	struct slskv_pairs  *buckets;		/* The buckets of key-value pairs */
	/* Read-only elements of the struct */
	u_long		    mask;		/* Hashmask used by the builtin hashtable */
	size_t		    valsize;		/* Size of values */
	enum slskv_policy   repl_policy;	/* Replace policy for adds */
	enum slskv_valtype  valtype;		/* Are values pointers or numbers? */
	/* Could become debug-only if it causes coherence traffic */
	size_t		    elems;		/* Number of elements in the table */
};

/* Hash function from keys to buckets. */
#define SLSKV_BUCKETNO(table, key) (((u_long) key & table->mask)) 

int slskv_create(struct slskv_table **tablep, enum slskv_policy policy, enum slskv_valtype type);
void slskv_destroy(struct slskv_table *table);
int slskv_find(struct slskv_table *table, uint64_t key, uintptr_t *value);
int slskv_add(struct slskv_table *table, uint64_t key, uintptr_t value);
int slskv_pop(struct slskv_table *table, uint64_t *key, uintptr_t *value);
void slskv_del(struct slskv_table *table, uint64_t key);


#define SLSKV_ITERDONE 1

/* 
 * Iterator used for dumping the contents of a key-value table. 
 * This data structure should be opaque to the users of the table.
 */
struct slskv_iter {
	int		    bucket;	/* The bucket currently being dumped */
	struct slskv_pair   *pair;	/* The KV pair currently being returned */
	struct slskv_table  *table;	/* The table being currently returned */
};

struct slskv_iter slskv_iterstart(struct slskv_table *table);
int slskv_itercont(struct slskv_iter *iter, uint64_t *key, uintptr_t *value);

/* Zone for the wrapper struct around KV pairs, used to enter them into a table. */
extern uma_zone_t slskv_zone;


#endif /* _SLSKV_H_ */
