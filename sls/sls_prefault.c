#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/signalvar.h>

#include <vm/vm.h>
#include <vm/vm_page.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_prefault.h"

int
slspre_create(size_t size, struct sls_prefault **slsprep)
{
	struct sls_prefault *slspre;

	slspre = malloc(sizeof(*slspre), M_SLSMM, M_WAITOK);
	slspre->pre_size = size;
	slspre->pre_map = bit_alloc(size, M_SLSMM, M_WAITOK);

	*slsprep = slspre;

	return (0);
}

void
slspre_destroy(struct sls_prefault *slspre)
{
	free(slspre->pre_map, M_SLSMM);
	free(slspre, M_SLSMM);
}

/*
 * Create a prefault vector for the object, but do not mark anything yet.
 * The SLS pager callback marks the pages it retrieves in the vector instead.
 */
int
slspre_vector_empty(
    uint64_t prefaultid, size_t size, struct sls_prefault **slsprep)
{
	struct sls_prefault *slspre;
	int error;

	/* Do not replace existing maps. */
	error = slskv_find(
	    slsm.slsm_prefault, prefaultid, (uintptr_t *)&slspre);
	if (error == 0)
		return (0);

	error = slspre_create(size, &slspre);
	if (error != 0)
		return (error);

	error = slskv_add(slsm.slsm_prefault, prefaultid, (uintptr_t)slspre);
	if (error != 0)
		slspre_destroy(slspre);

	if (slsprep != NULL)
		*slsprep = slspre;

	return (error);
}

/*
 * Create a prefault vector for the object and mark in it the current resident
 * pages.
 */
int
slspre_vector_populated(uint64_t prefaultid, vm_object_t obj)
{
	struct sls_prefault *slspre;
	size_t size;
	vm_page_t m;
	int error;

	if (obj == NULL)
		return (0);

	if (obj->size == 0)
		return (0);

	/* Do not replace existing maps. */
	error = slskv_find(
	    slsm.slsm_prefault, prefaultid, (uintptr_t *)&slspre);
	if (error == 0)
		return (0);

	size = obj->size;
	error = slspre_create(size, &slspre);
	if (error != 0)
		return (error);

	VM_OBJECT_RLOCK(obj);

	/*
	 * We currently do not handle shrinking or expanding objects.
	 * This should not be an issue with these objects, since they
	 * are part of the in-memory representation of the checkpoint,
	 * and as such are not modified by running applications.
	 */
	KASSERT(size == obj->size, ("object size changed"));

	TAILQ_FOREACH (m, &obj->memq, listq) {
		if (m->pindex >= obj->size)
			continue;
		bit_set(slspre->pre_map, m->pindex);
	}
	VM_OBJECT_RUNLOCK(obj);

	error = slskv_add(slsm.slsm_prefault, prefaultid, (uintptr_t)slspre);
	if (error != 0)
		slspre_destroy(slspre);

	return (error);
}

/*
 * Mark a range of pages in the object in the prefault vector.
 */
void
slspre_mark(uint64_t prefaultid, vm_pindex_t start, vm_pindex_t stop)
{
	struct sls_prefault *slspre;
	int error;

	error = slskv_find(
	    slsm.slsm_prefault, prefaultid, (uintptr_t *)&slspre);
	if (error != 0)
		return;

	bit_nset(slspre->pre_map, start, stop);
}
