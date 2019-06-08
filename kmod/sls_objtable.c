#include <sys/param.h>

#include <sys/conf.h>
#include <sys/hash.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

#include "sls_objtable.h"
#include "sls_dump.h"
#include "slsmm.h"

int 
sls_objtable_init(struct sls_objtable *objtable)
{
	objtable->objects = hashinit(HASH_MAX, M_SLSMM, &objtable->hashmask);
	if (objtable->objects == NULL)
		return ENOMEM;

	return 0;
}

void
sls_objtable_fini(struct sls_objtable *objtable)
{
	int i;
	struct vmobj_pair *pair;
	struct vmobj_pairs *bucket;

	for (i = 0; i <= objtable->hashmask; i++) {
		bucket = &objtable->objects[i];
		/* 
		 * XXX replace with LIST_FOREACH_SAFE 
		 * (deferred because we are currently changing a lot already)
		 */
		while (!LIST_EMPTY(bucket)) {
			pair = LIST_FIRST(bucket);
			LIST_REMOVE(pair, next);
			free(pair, M_SLSMM);
		}
	}

	hashdestroy(objtable->objects, M_SLSMM, objtable->hashmask);
}

vm_object_t
sls_objtable_find(vm_object_t obj, struct sls_objtable *objtable)
{
	struct vmobj_pair *pair;
	struct vmobj_pairs *bucket;

	bucket = &objtable->objects[(uintptr_t) obj & objtable->hashmask];

	LIST_FOREACH(pair, bucket, next)
	    if (pair->original == obj)
		return pair->restored;

	return NULL;
}

void
sls_objtable_add(vm_object_t original, vm_object_t restored, struct sls_objtable *objtable)
{
	struct vmobj_pairs *bucket;
	struct vmobj_pair *pair;

	pair = malloc(sizeof(*pair), M_SLSMM, M_WAITOK);
	pair->original = original;
	pair->restored = restored;

	bucket = &objtable->objects[(uintptr_t) original & objtable->hashmask];

	/* 
	 * We assume no duplicates. If we even reach this 
	 * point with a duplicate entry, we have created
	 * two objects for one original, so we are already
	 * in bug territory.
	 */
	LIST_INSERT_HEAD(bucket, pair, next);
}
