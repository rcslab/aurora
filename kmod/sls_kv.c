#include <sys/param.h>
#include <sys/lock.h>
#include <sys/queue.h>

#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/sbuf.h>
#include <sys/systm.h>

#include <vm/uma.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_mm.h"

/* Hash function from keys to buckets. */
#define SLSKV_BUCKETNO(table, key) (((u_long) key & table->mask))

static uma_zone_t slskvpair_zone = NULL;
uma_zone_t slskv_zone = NULL;

static int
slskv_zone_ctor(void *mem, int size, void *args __unused, int flags __unused)
{
	/* XXX Put in KASSERTs */
	return (0);
}

static void
slskv_zone_dtor(void *mem, int size, void *args __unused)
{
	struct slskv_table *table;
	struct slskv_pair *kv, *tmpkv;
	int i;

	table = (struct slskv_table *) mem;

	/* XXX Put in KASSERTs */
	/* Iterate all buckets. */
	for (i = 0; i <= table->mask; i++) {
		LIST_FOREACH_SAFE(kv, &table->buckets[i], next, tmpkv) {
			/*
			 * Remove all elements from each bucket and free them.
			 * We are also responsible for freeing the values themselves.
			 */
			LIST_REMOVE(kv, next);
			uma_zfree(slskvpair_zone, kv);
		}
	}
}

static int
slskv_zone_init(void *mem, int size, int flags __unused)
{
	struct slskv_table *table;
	int i;

	table = (struct slskv_table *) mem;

	/* Create the buckets using the existing kernel functions. */
	table->buckets = hashinit(SLSKV_BUCKETS, M_SLSMM, &table->mask);
	if (table->buckets == NULL) {
		free(table, M_SLSMM);
		return (ENOMEM);
	}

	/* Initialize the mutexes. */
	for (i = 0; i < SLSKV_BUCKETS; i++)
		mtx_init(&table->mtx[i], "slskvmtx", NULL, MTX_DEF);

	/* Shared lock for mutation, exclusive for iteration. */
	sx_init(&table->sx, "slskvsx");

	table->data = NULL;

	return (0);
}

static void
slskv_zone_fini(void *mem, int size)
{
	struct slskv_table *table;
	int i;

	table = (struct slskv_table *) mem;

	/* Destroy the hashtable itself. */
	hashdestroy(table->buckets, M_SLSMM, table->mask);

	/* Destroy the lock. */
	for (i = 0; i < SLSKV_BUCKETS; i++)
		mtx_destroy(&table->mtx[i]);

	sx_destroy(&table->sx);
}

int
slskv_init(void)
{
	slskv_zone = uma_zcreate("SLS tables",
	    sizeof(struct slskv_table), slskv_zone_ctor, slskv_zone_dtor,
	    slskv_zone_init, slskv_zone_fini,
	    UMA_ALIGNOF(struct slskv_table), 0);
	if (slskv_zone == NULL)
		return (ENOMEM);

	slskvpair_zone = uma_zcreate("SLS table pairs",
	    sizeof(struct slskv_pair), NULL, NULL, NULL,
	    NULL, UMA_ALIGNOF(struct slskv_pair), 0);
	if (slskvpair_zone == NULL) {
		uma_zdestroy(slskv_zone);
		slskv_zone = NULL;
		return (ENOMEM);
	}

	return (0);
}

void
slskv_fini(void)
{
	if (slskv_zone != NULL)
		uma_zdestroy(slskv_zone);

	if (slskvpair_zone != NULL)
		uma_zdestroy(slskvpair_zone);
}

int
slskv_create(struct slskv_table **tablep)
{
	*tablep = uma_zalloc(slskv_zone, M_WAITOK);
	if (*tablep == NULL)
		return (ENOMEM);

	return (0);
}

void
slskv_destroy(struct slskv_table *table)
{
	uma_zfree(slskv_zone, table);
}

/* Find a value corresponding to a 64bit key. */
int
slskv_find_unlocked(struct slskv_table *table, uint64_t key, uintptr_t *value)
{
	struct slskv_pair *kv;

	mtx_lock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	/* Traverse the bucket for the specific key. */
	LIST_FOREACH(kv, &table->buckets[SLSKV_BUCKETNO(table, key)], next) {
		if (kv->key == key) {
			mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
			*value = kv->value;
			return (0);
		}
	}

	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	/* We failed to find the key. */
	return (EINVAL);
}

int
slskv_find(struct slskv_table *table, uint64_t key, uintptr_t *value)
{
	int error;

	sx_slock(&table->sx);
	error = slskv_find_unlocked(table, key, value);
	sx_sunlock(&table->sx);

	return (error);
}

int
slskv_add_unlocked(struct slskv_table *table, uint64_t key, uintptr_t value)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *newkv, *kv;

	newkv = uma_zalloc(slskvpair_zone, M_WAITOK);
	newkv->key = key;
	newkv->value = value;

	/* Get the bucket for the key. */
	bucket = &table->buckets[SLSKV_BUCKETNO(table, key)];

	mtx_lock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	/* Try to find existing instances of the key. */
	LIST_FOREACH(kv, bucket, next) {
		/* We found the key, so we cannot insert. */
		if (kv->key == key) {
			uma_zfree(slskvpair_zone, newkv);
			mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
			return (EINVAL);
		}
	}

	/* We didn't find the key, so we are free to insert. */
	LIST_INSERT_HEAD(bucket, newkv, next);
	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	return (0);
}

/* Add a new value to the hashtable. Duplicates are not allowed. */
int
slskv_add(struct slskv_table *table, uint64_t key, uintptr_t value)
{
	int error;

	sx_slock(&table->sx);
	error = slskv_add_unlocked(table, key, value);
	sx_sunlock(&table->sx);

	return (error);
}

/* 
 * Delete all instances of a key. 
 */
void
slskv_del(struct slskv_table *table, uint64_t key)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *kv, *tmpkv;

	sx_slock(&table->sx);
	mtx_lock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	/* Get the bucket for the key and traverse it. */
	bucket = &table->buckets[SLSKV_BUCKETNO(table, key)];

	LIST_FOREACH_SAFE(kv, bucket, next, tmpkv) {
		/* 
		 * We found an instance of the key.
		 * Remove it and erase it and its value. 
		 */
		if (kv->key == key) {
			LIST_REMOVE(kv, next);
			uma_zfree(slskvpair_zone, kv);

			/* We remove at most one pair. */
			break;
		}
	}

	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	sx_sunlock(&table->sx);
}

static int
slskv_pop_unlocked(struct slskv_table *table, uint64_t *key, uintptr_t *value)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *kv;
	int error = EINVAL, i;

	mtx_lock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	for (i = 0; i <= table->mask; i++) {
		bucket = &table->buckets[i];
		if (!LIST_EMPTY(bucket)) {
			kv = LIST_FIRST(bucket);

			*key = kv->key;
			*value = kv->value;

			LIST_REMOVE(kv, next);
			uma_zfree(slskvpair_zone, kv);
			error = 0;

			break;
		}
	}

	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	return (error);
}

/*
 * Randomly grab an element from the table, remove it and
 * return it. If the table is empty, return an error.
 */
int
slskv_pop(struct slskv_table *table, uint64_t *key, uintptr_t *value)
{
	int error;

	sx_xlock(&table->sx);
	error = slskv_pop_unlocked(table, key, value);
	sx_xunlock(&table->sx);

	return (error);
}

/*
 * Find the first nonempty bucket of the table.
 * If the whole table is empty, stop searching
 * after inspecting the last bucket.
 */
static void
slskv_iternextbucket(struct slskv_iter *iter)
{
	int curbucket;

	/* Find the first nonempty bucket. */
	for (;;) {
		curbucket = iter->bucket;

		/* If the index is out of bounds we're done. */
		if (curbucket > iter->table->mask)
			break;

		/* Have the global index point to the next unchecked bucket. */
		iter->bucket += 1;

		/* Inspect the current bucket, break if we find any data. */
		if (!LIST_EMPTY(&iter->table->buckets[curbucket]))
			break;

	}


	/* If there is a nonempty bucket, the iterator points to its first element. */
	if (curbucket <= iter->table->mask)
		iter->pair = LIST_FIRST(&iter->table->buckets[curbucket]);
}

/*
 * Return an iterator for the given key-value table. Note that we cannot 
 * iterate recursively or serialize, and any operations on the table
 * should be done unlocked (finds always make sense, additions sometimes,
 * deletions are dangerous and are not available).
 */
struct slskv_iter 
slskv_iterstart(struct slskv_table *table)
{
	struct slskv_iter iter;

	iter.bucket = 0;
	iter.pair = NULL;
	iter.table = table;

	/* Find the next valid element, if it exists. */
	sx_xlock(&iter.table->sx);
	slskv_iternextbucket(&iter);

	return iter;
}

/*
 * Return the next pair of the key-value table.
 * Signal to the caller if the iteration has ended.
 */
int
slskv_itercont(struct slskv_iter *iter, uint64_t *key, uintptr_t *value)
{

	if (iter->pair == NULL) {

		/* We need to find the another bucket. */
		slskv_iternextbucket(iter);

		/* If we have no more buckets to look at, iteration is done. */
		if (iter->bucket > iter->table->mask) {
			sx_xunlock(&iter->table->sx);
			return SLSKV_ITERDONE;
		}
	} 

	/* Export the found pair to the caller. */
	*key = iter->pair->key;
	*value= iter->pair->value;

	/* Point to the next pair. */
	iter->pair = LIST_NEXT(iter->pair, next); 

	return (0);
}

/* Abort the iteration. Needed to release the lock. */
void
slskv_iterabort(struct slskv_iter *iter)
{
	sx_xunlock(&iter->table->sx);
	bzero(iter, sizeof(*iter));
}

int
slskv_serial_unlocked(struct slskv_table *table, struct sbuf *sb)
{
	uintptr_t value;
	uint64_t key;
	int error;

	/* Iterate the hashtable, saving the key-value pairs. */
	KV_FOREACH_POP_UNLOCKED(table, key, value) {
		error = sbuf_bcat(sb, (void *) &key, sizeof(key));
		if (error != 0) {
			sx_xunlock(&table->sx);
			return (error);
		}

		sbuf_bcat(sb, (void *) &value, sizeof(value));
		if (error != 0) {
			sx_xunlock(&table->sx);
			return (error);
		}
	}

	return (0);
}

/* 
 * Serialize a table into a linear buffer. 
 * Used for exporting the table to disk.
 */
int
slskv_serial(struct slskv_table *table, struct sbuf *sb)
{
	int error;

	sx_xlock(&table->sx);
	error = slskv_serial_unlocked(table, sb);
	sx_xunlock(&table->sx);
	return (error);
}

/*
 * Deserialize a table from a linear buffer.
 * Used from importing the table from disk.
 */
int
slskv_deserial(char *buf, size_t len, struct slskv_table **tablep)
{
	struct slskv_table *table;
	uintptr_t value;
	char *pairaddr;
	uint64_t key;
	int i, error;
	size_t pairs;

	KASSERT(buf != NULL, ("buffer is not finalized"));

	error = slskv_create(&table);
	if (error != 0)
		return (error);

	/* Find out how many pairs there are. */
	pairs = len / (sizeof(uint64_t) + sizeof(uintptr_t));
	for (i = 0; i < pairs; i++) {
		pairaddr = &buf[i * (sizeof(uint64_t) + sizeof(uintptr_t))];
		memcpy(&key, (void *) pairaddr, sizeof(key));
		memcpy(&value, (void *) (pairaddr + sizeof(key)), sizeof(value));

		error = slskv_add(table, key, value);
		if (error != 0) {
			slskv_destroy(table);
			return (error);
		}
	}

	/* Export the resulting table to the caller. */
	*tablep = table;

	return (0);
}

int 
slsset_find(slsset *table, uint64_t key)
{
	uintptr_t nothing;

	return (slskv_find(table, key, &nothing));
}

int 
slsset_find_unlocked(slsset *table, uint64_t key)
{
	uintptr_t nothing;

	return (slskv_find_unlocked(table, key, &nothing));
}

int
slsset_add(slsset *table, uint64_t key)
{
	return (slskv_add(table, key, (uintptr_t) key));
}

int
slsset_add_unlocked(slsset *table, uint64_t key)
{
	return (slskv_add_unlocked(table, key, (uintptr_t) key));
}

int
slsset_pop(slsset *table, uint64_t *key)
{
	return (slskv_pop(table, key, (uintptr_t *) key));
}
