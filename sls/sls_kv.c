#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/sbuf.h>

#include <vm/uma.h>

#include "sls_internal.h"
#include "sls_kv.h"

/*
 * NOTE: Iterating through a table while doing other operations
 * on it is undefined. This is a deliberate inconsistency in the design of
 * the data structure which does not affect us because at no point during
 * normal operation are the two operations supposed to overlap. We still
 * use per-bucket mutexes. Even if an add/delete overlaps with an
 * iteration there is no possibility of data corruption. What _does_ happen
 * is that iteration operates on an inconsistent view of the KV store, and
 * might thus return erroneous results. The same holds for a popall operation.
 */

/* Hash function from keys to buckets. */
#define SLSKV_BUCKETNO(table, key) (((u_long)key & table->mask))
#define SLSKVPAIR_ZONEWARM (8192)
#define SLSKV_ZONEWARM (256)

static uma_zone_t slskvpair_zone = NULL;
uma_zone_t slskv_zone = NULL;

int slskv_count = 0;

static int
slskv_zone_ctor(void *mem, int size, void *args __unused, int flags __unused)
{
	/* XXX Put in KASSERTs */
	atomic_add_int(&slskv_count, 1);
	return (0);
}

static void
slskv_zone_dtor(void *mem, int size, void *args __unused)
{
	struct slskv_table *table;
	struct slskv_pair *kv, *tmpkv;
	int i;

	table = (struct slskv_table *)mem;

	/* Iterate all buckets. */
	for (i = 0; i <= table->mask; i++) {
		LIST_FOREACH_SAFE (kv, &table->buckets[i], next, tmpkv) {
			/*
			 * Remove all elements from each bucket and free them.
			 * We are also responsible for freeing the values
			 * themselves.
			 */
			LIST_REMOVE(kv, next);
			uma_zfree(slskvpair_zone, kv);
		}
	}

	atomic_add_int(&slskv_count, -1);
}

static int
slskv_zone_init(void *mem, int size, int flags __unused)
{
	struct slskv_table *table;
	int i;

	table = (struct slskv_table *)mem;

	/* Create the buckets using the existing kernel functions. */
	table->buckets = hashinit(SLSKV_BUCKETS, M_SLSMM, &table->mask);
	if (table->buckets == NULL)
		return (ENOMEM);

	/* Initialize the mutexes. */
	for (i = 0; i < SLSKV_BUCKETS; i++)
		mtx_init(&table->mtx[i], "slskvmtx", NULL, MTX_SPIN);

	table->data = NULL;

	return (0);
}

static void
slskv_zone_fini(void *mem, int size)
{
	struct slskv_table *table;
	int i;

	table = (struct slskv_table *)mem;

	/* Destroy the hashtable itself. */
	hashdestroy(table->buckets, M_SLSMM, table->mask);

	/* Destroy the lock. */
	for (i = 0; i < SLSKV_BUCKETS; i++)
		mtx_destroy(&table->mtx[i]);
}

int
slskv_init(void)
{
	slskv_zone = uma_zcreate("slstable", sizeof(struct slskv_table),
	    slskv_zone_ctor, slskv_zone_dtor, slskv_zone_init, slskv_zone_fini,
	    UMA_ALIGNOF(struct slskv_table), 0);
	if (slskv_zone == NULL)
		return (ENOMEM);

	uma_prealloc(slskv_zone, SLSKVPAIR_ZONEWARM);

	slskvpair_zone = uma_zcreate("slkvpair", sizeof(struct slskv_pair),
	    NULL, NULL, NULL, NULL, UMA_ALIGNOF(struct slskv_pair), 0);
	if (slskvpair_zone == NULL) {
		uma_zdestroy(slskv_zone);
		slskv_zone = NULL;
		return (ENOMEM);
	}

	uma_prealloc(slskvpair_zone, SLSKVPAIR_ZONEWARM);

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
	*tablep = uma_zalloc(slskv_zone, M_NOWAIT);
	if (*tablep == NULL)
		return (ENOMEM);

	return (0);
}

void
slskv_destroy(struct slskv_table *table)
{
	uint64_t key;
	uintptr_t value;

	/* Pop all elements from the table */
	KV_FOREACH_POP(table, key, value);

	uma_zfree(slskv_zone, table);
}

/* Find a value corresponding to a 64bit key. */
static int
slskv_find_unlocked(struct slskv_table *table, uint64_t key, uintptr_t *value)
{
	struct slskv_pair *kv;

	/* Traverse the bucket for the specific key. */
	LIST_FOREACH (kv, &table->buckets[SLSKV_BUCKETNO(table, key)], next) {
		if (kv->key == key) {
			*value = kv->value;
			return (0);
		}
	}

	/* We failed to find the key. */
	return (EINVAL);
}

int
slskv_find(struct slskv_table *table, uint64_t key, uintptr_t *value)
{
	int error;

	mtx_lock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	error = slskv_find_unlocked(table, key, value);
	mtx_unlock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	return (error);
}

static int
slskv_add_unlocked(struct slskv_table *table, struct slskv_pair *newkv)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *kv;

	/* Get the bucket for the key. */

	bucket = &table->buckets[SLSKV_BUCKETNO(table, newkv->key)];

	/* Try to find existing instances of the key. */
	LIST_FOREACH (kv, bucket, next) {
		/* We found the key, so we cannot insert. */
		if (kv->key == newkv->key)
			return (EINVAL);
	}

	/* We didn't find the key, so we are free to insert. */
	LIST_INSERT_HEAD(bucket, newkv, next);

	return (0);
}

/* Add a new value to the hashtable. Duplicates are not allowed. */
int
slskv_add(struct slskv_table *table, uint64_t key, uintptr_t value)
{
	struct slskv_pair *newkv;
	int error;

	newkv = uma_zalloc(slskvpair_zone, M_NOWAIT);
	if (newkv == NULL)
		return (ENOMEM);

	newkv->key = key;
	newkv->value = value;

	mtx_lock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	error = slskv_add_unlocked(table, newkv);
	mtx_unlock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	if (error != 0)
		uma_zfree(slskvpair_zone, newkv);

	return (error);
}

static struct slskv_pair *
slskv_del_unlocked(struct slskv_table *table, uint64_t key)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *kv, *tmpkv;

	/* Get the bucket for the key and traverse it. */
	bucket = &table->buckets[SLSKV_BUCKETNO(table, key)];

	LIST_FOREACH_SAFE (kv, bucket, next, tmpkv) {
		/*
		 * We found an instance of the key.
		 * Remove it and erase it and its value.
		 */
		if (kv->key == key) {
			LIST_REMOVE(kv, next);
			return (kv);
		}
	}

	return (NULL);
}

/*
 * Delete all instances of a key.
 */
void
slskv_del(struct slskv_table *table, uint64_t key)
{
	struct slskv_pair *kv;

	mtx_lock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	kv = slskv_del_unlocked(table, key);
	mtx_unlock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	uma_zfree(slskvpair_zone, kv);
}

static struct slskv_pair *
slskv_pop_unlocked(struct slskv_table *table, uint64_t *key, uintptr_t *value)
{
	struct slskv_pairs *bucket;
	struct slskv_pair *kv;
	int i;

	for (i = 0; i <= table->mask; i++) {
		bucket = &table->buckets[i];
		if (!LIST_EMPTY(bucket)) {
			kv = LIST_FIRST(bucket);

			*key = kv->key;
			*value = kv->value;

			LIST_REMOVE(kv, next);
			return (kv);
		}
	}

	return (NULL);
}

/*
 * Randomly grab an element from the table, remove it and
 * return it. If the table is empty, return an error.
 */
int
slskv_pop(struct slskv_table *table, uint64_t *key, uintptr_t *value)
{
	struct slskv_pair *kv;

	mtx_lock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);
	kv = slskv_pop_unlocked(table, key, value);
	mtx_unlock_spin(&table->mtx[SLSKV_BUCKETNO(table, key)]);

	if (kv != NULL) {
		uma_zfree(slskvpair_zone, kv);
		return (0);
	}

	return (EINVAL);
}

/*
 * Find the first nonempty bucket of the table.
 * If the whole table is empty, stop searching
 * after inspecting the last bucket.
 */
static void
slskv_iternextbucket(struct slskv_iter *iter)
{
	/* Find the first nonempty bucket. */
	for (;;) {

		/* If the index is out of bounds we're done. */
		if (iter->bucket > iter->table->mask)
			return;

		/* Inspect the current bucket, break if we find any data. */
		if (!LIST_EMPTY(&iter->table->buckets[iter->bucket]))
			break;

		/* Have the global index point to the next unchecked bucket. */
		iter->bucket += 1;
	}

	/* If there is a nonempty bucket, point to its first element. */
	iter->pair = LIST_FIRST(&iter->table->buckets[iter->bucket]);
	iter->bucket += 1;
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

	KASSERT(table != NULL, ("iterating on NULL table\n"));

	iter.table = table;
	iter.bucket = 0;
	iter.pair = NULL;

	return (iter);
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
		if (iter->pair == NULL) {
			KASSERT(iter->bucket > iter->table->mask,
			    ("stopped iteration on bucket %d", iter->bucket));
			return (SLSKV_ITERDONE);
		}
	}

	/* Export the found pair to the caller. */
	*key = iter->pair->key;
	*value = iter->pair->value;

	/* Point to the next pair. */
	iter->pair = LIST_NEXT(iter->pair, next);

	return (0);
}

/* Abort the iteration. Needed to release the lock. */
void
slskv_iterabort(struct slskv_iter *iter)
{
	bzero(iter, sizeof(*iter));
}

int
slsset_find(slsset *table, uint64_t key)
{
	uintptr_t nothing;

	return (slskv_find(table, key, &nothing));
}

int
slsset_add(slsset *table, uint64_t key)
{
	return (slskv_add(table, key, (uintptr_t)key));
}

int
slsset_pop(slsset *table, uint64_t *key)
{
	return (slskv_pop(table, key, (uintptr_t *)key));
}
