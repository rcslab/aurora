#ifndef _SLSKV_H_
#define _SLSKV_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/sx.h>

#include <vm/uma.h>

#include <machine/param.h>

/* The number of buckets created. */
#define SLSKV_BUCKETS (16)

/*
 * Generic hashtable data structure. Built on top
 * of FreeBSD's existing hashtable functions.
 */
struct slskv_pair {
	uint64_t key;		     /* A 64bit key */
	uintptr_t value;	     /* Pointer to a value of arbitrary size,
					or possibly the value itself. */
	LIST_ENTRY(slskv_pair) next; /* Used for chaining in the hashtable */
};

LIST_HEAD(slskv_pairs, slskv_pair); /* A bucket of the hashtable */

/* The main key-value table. */
struct slskv_table {
	struct mtx_padalign mtx[SLSKV_BUCKETS]; /* Per-bucket locking */
	struct slskv_pairs *buckets; /* The buckets of key-value pairs */
	/* Read-only elements of the struct */
	u_long mask; /* Hashmask used by the builtin hashtable */
	void *data;  /* Private data */
};

int slskv_create(struct slskv_table **tablep);
void slskv_destroy(struct slskv_table *table);
int slskv_find(struct slskv_table *table, uint64_t key, uintptr_t *value);
int slskv_add(struct slskv_table *table, uint64_t key, uintptr_t value);
int slskv_pop(struct slskv_table *table, uint64_t *key, uintptr_t *value);
void slskv_del(struct slskv_table *table, uint64_t key);

/*
 * Implementation of a hash set on top of the table. In order to reuse code,
 * we still store the data in struct slskv_pair objects, even though that means
 * we waste 64 bits for every element. However, the table is not used to store
 * large amounts of data, so the memory footprint is still low.
 */

typedef struct slskv_table slsset;

#define slsset_create(tablep) (slskv_create(tablep))
#define slsset_destroy(table) (slskv_destroy(table))
#define slsset_del(table, key) (slskv_del(table, key))

int slsset_find(slsset *table, uint64_t key);
int slsset_add(slsset *table, uint64_t key);
int slsset_pop(slsset *table, uint64_t *key);

#define SLSKV_ITERDONE 1

/* Initialization/finalization routines for the subsystem. */
int slskv_init(void);
void slskv_fini(void);

/*
 * Iterator used for dumping the contents of a key-value table.
 * This data structure should be opaque to the users of the table.
 */
struct slskv_iter {
	int bucket;		   /* The bucket currently being dumped */
	struct slskv_pair *pair;   /* The KV pair currently being returned */
	struct slskv_table *table; /* The table being currently returned */
};

struct slskv_iter slskv_iterstart(struct slskv_table *table);
int slskv_itercont(struct slskv_iter *iter, uint64_t *key, uintptr_t *value);
void slskv_iterabort(struct slskv_iter *iter);

#define KV_FOREACH_POP(table, kvkey, kvvalue)                \
	_Static_assert(sizeof(kvkey) == sizeof(uint64_t),    \
	    "popping into variable of the wrong size");      \
	_Static_assert(sizeof(kvvalue) == sizeof(uintptr_t), \
	    "popping into variable of the wrong size");      \
	while (slskv_pop((table), (uint64_t *)&(kvkey),      \
		   (uintptr_t *)&(kvvalue)) == 0)

#define KVSET_FOREACH_POP(settable, setvalue)                \
	_Static_assert(sizeof(setvalue) == sizeof(uint64_t), \
	    "popping into variable of the wrong size");      \
	while (slsset_pop((settable), (uint64_t *)&(setvalue)) == 0)

#define KV_FOREACH(table, iter, kvkey, kvvalue)              \
	_Static_assert(sizeof(kvkey) == sizeof(uint64_t),    \
	    "popping into variable of the wrong size");      \
	_Static_assert(sizeof(kvvalue) == sizeof(uintptr_t), \
	    "popping into variable of the wrong size");      \
	for ((iter) = slskv_iterstart(table);                \
	     slskv_itercont(&(iter), (uint64_t *)&(kvkey),   \
		 (uintptr_t *)&(kvvalue)) != SLSKV_ITERDONE;)

#define KVSET_FOREACH(settable, iter, setvalue)               \
	_Static_assert(sizeof(setvalue) == sizeof(uint64_t),  \
	    "popping into variable of the wrong size");       \
	for ((iter) = slskv_iterstart(settable);              \
	     slskv_itercont(&(iter), (uint64_t *)&(setvalue), \
		 (uintptr_t *)&(setvalue)) != SLSKV_ITERDONE;)

#define KV_ABORT(iter)                      \
	do {                                \
		(slskv_iterabort(&(iter))); \
	} while (0)

/* Zone for the key value tables. */
extern uma_zone_t slskv_zone;
extern int slskv_count;

#endif /* _SLSKV_H_ */
