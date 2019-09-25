#include <sys/param.h>

#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/queue.h>

#include <sys/lock.h>
#include <sys/mutex.h>

#include <vm/uma.h>

#include "slskv.h"
#include "slsmm.h"
#include "sls.h"

uma_zone_t slskv_zone;

/* 
 * Create a table with values of the specified size.
 */
int 
slskv_create(struct slskv_table **tablep, enum slskv_policy policy, enum slskv_valtype type)
{
	struct slskv_table *table;
	int i;

	table = malloc(sizeof(*table), M_SLSMM, M_WAITOK);

	/* Create the buckets using the existing kernel functions. */
	table->buckets = hashinit(SLSKV_BUCKETS, M_SLSMM, &table->mask);
	if (table->buckets == NULL) {
	    free(table, M_SLSMM);
	    return ENOMEM;
	}

	/* Initialize the mutex. */
	for (i = 0; i < SLSKV_BUCKETS; i++)
	    mtx_init(&table->mtx[i], "slskv", NULL, MTX_DEF);

	table->repl_policy = policy;
	table->valtype = type;
	table->elems = 0;

	/* Export the table. */
	*tablep = table;

	return 0;
}

/*
 * Destroy a hashtable, freeing all key-value pairs in the process.
 * This function is not protected by the table lock, since that 
 * will be destroyed by the end. We use SLOS-wide functions for that. 
 */
void
slskv_destroy(struct slskv_table *table)
{
	struct slskv_pair *kv, *tmpkv;
	int i;

	/* Iterate all buckets. */
	for (i = 0; i <= table->mask; i++) {
	    LIST_FOREACH_SAFE(kv, &table->buckets[i], next, tmpkv) {
		/* 
		 * Remove all elements from each bucket and free them. 
		 * We are also responsible for freeing the values themselves. 
		 */
		LIST_REMOVE(kv, next);
		if (table->valtype == SLSKV_VALPTR)
		    free((void *) kv->value, M_SLSMM);
		uma_zfree(slskv_zone, kv);
	    }
	}

	/* Destroy the hashtable itself. */
	hashdestroy(table->buckets, M_SLSMM, table->mask);

	/* Destroy the lock. */
	for (i = 0; i < SLSKV_BUCKETS; i++)
	    mtx_destroy(&table->mtx[i]);

	/* Free the table itself. */
	free(table, M_SLSMM);
}

/* Find a value corresponding to a 64bit key. */
int
slskv_find(struct slskv_table *table, uint64_t key, uintptr_t *value)
{
	struct slskv_pair *kv;

	mtx_lock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	/* Traverse the bucket for the specific key. */
	LIST_FOREACH(kv, &table->buckets[SLSKV_BUCKETNO(table, key)], next) {
	    if (kv->key == key) {
		mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
		*value = kv->value;
		return 0;
	    }
	}

	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	/* We failed to find the key. */
	return EINVAL;
}

/* 
 * Add a new value to the hashtable. Inserting the new value may fail
 * depending on whether the key already exists and the replace policy.
 */
int
slskv_add(struct slskv_table *table, uint64_t key, uintptr_t value)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *newkv, *kv, *tmpkv;

	newkv = uma_zalloc(slskv_zone, M_WAITOK);
	newkv->key = key;
	newkv->value = value;

	/* Get the bucket for the key. */
	bucket = &table->buckets[SLSKV_BUCKETNO(table, key)];

	mtx_lock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	switch (table->repl_policy) {
	/* We don't care about multiples, so we blindly insert the new value. */
	case SLSKV_MULTIPLES:
	    LIST_INSERT_HEAD(bucket, newkv, next);
	    table->elems += 1;
	    mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	    return 0;

	
	case SLSKV_NOREPLACE:
	    /* Try to find existing instances of the key. */
	    LIST_FOREACH(kv, bucket, next) {
		/* We found the key, so we cannot insert. */
		if (kv->key == key) {
		    uma_zfree(slskv_zone, kv);
		    mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
		    return EINVAL;
		}
	    }

	    /* We didn't find the key, so we are free to insert. */
	    LIST_INSERT_HEAD(bucket, newkv, next);
	    table->elems += 1;
	    mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	    return 0;

	case SLSKV_REPLACE:
	    /* Try to find existing instances of the key. */
	    LIST_FOREACH_SAFE(kv, bucket, next, tmpkv) {
		/* 
		 * We found an old instance of the key.
		 * Remove it and erase it and its value. 
		 */
		if (kv->key == key) {
		    LIST_REMOVE(kv, next);
		    if (table->valtype == SLSKV_VALPTR)
			free((void *) kv->value, M_SLSMM);

		    uma_zfree(slskv_zone, kv);
		}
	    }

	    /* We destroyed all old instances, insert the new one. */
	    LIST_INSERT_HEAD(bucket, newkv, next);
	    table->elems += 1;

	    mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	    return 0;
	}
}

/* 
 * Delete all instances of a key. 
 */
void
slskv_del(struct slskv_table *table, uint64_t key)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *kv, *tmpkv;

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
		if (table->valtype == SLSKV_VALPTR)
		    free((void *) kv->value, M_SLSMM);
		uma_zfree(slskv_zone, kv);
		table->elems -= 1;

		/* We remove at most one pair. */
		break;
	    }
	}

	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);
}

/*
 * Randomly grab an element from the table, remove it and
 * return it. If the table is empty, return an error.
 */
int
slskv_pop(struct slskv_table *table, uint64_t *key, uint64_t *value)
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
		error = 0;

		break;
	    }
	}

	mtx_unlock(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	return error;
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
 * Return an iterator for the given key-value table.
 * NOTE: The iteration operation DOES NOT TAKE ANY LOCKS.
 * That means that iteration should never happen concurrently
 * with operations that modify the table. The alternative would
 * be having a variable that normal operations check before beginning
 * execution; if it is set, they wait until it is free. This is essentiallly
 * a lock that is taken by the iterator, and checked, but _never_ taken, by the
 * rest of the operations. If the lock was taken by the operations, then we could
 * end up exclusively using the global lock for prolonged periods of time, even if
 * no iteration is taking place.
 */
struct slskv_iter 
slskv_iterstart(struct slskv_table *table)
{
	struct slskv_iter iter;

	iter.bucket = 0;
	iter.pair = NULL;
	iter.table = table;

	/* Find the next valid element, if it exists. */
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
	    if (iter->bucket > iter->table->mask)
		return SLSKV_ITERDONE;
	} 

	/* Export the found pair to the caller. */
	*key = iter->pair->key;
	*value= iter->pair->value;

	/* Point to the next pair. */
	iter->pair = LIST_NEXT(iter->pair, next); 

	return 0;
}
