#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/signalvar.h>

#include <vm/vm.h>
#include <vm/vm_page.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_prefault.h"

#define MAXPRE_PAGES (MAXBCACHEBUF / PAGE_SIZE)

uint64_t sls_prefault_vnios;
uint64_t sls_prefault_vnpages;

int
slspre_create(uint64_t slsid, size_t size, struct sls_prefault **slsprep)
{
	struct sls_prefault *slspre;

	slspre = malloc(sizeof(*slspre), M_SLSMM, M_WAITOK);
	slspre->pre_slsid = slsid;
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

	error = slspre_create(prefaultid, size, &slspre);
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

	/* Replace existing maps. */
	if (slskv_find(slsm.slsm_prefault, prefaultid, (uintptr_t *)&slspre) ==
	    0) {
		slskv_del(slsm.slsm_prefault, prefaultid);
		KASSERT(slspre->pre_size == obj->size,
		    ("prefault vector %lx has size %ld insteda of expected %ld",
			prefaultid, slspre->pre_size, obj->size));

		slspre_destroy(slspre);
		slspre = NULL;
	}

	size = obj->size;
	error = slspre_create(prefaultid, size, &slspre);
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
		KASSERT(m->pindex < slspre->pre_size, ("out of bounds"));
		bit_set(slspre->pre_map, m->pindex);
	}
	VM_OBJECT_RUNLOCK(obj);

	error = slskv_add(slsm.slsm_prefault, prefaultid, (uintptr_t)slspre);
	if (error != 0)
		slspre_destroy(slspre);
	KASSERT(size == obj->size, ("mismatch %ld vs %ld", size, obj->size));

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

/*
 * Populate the page cache with the pages to be prefaulted.
 */
static int
slspre_getpages(struct vnode *vp, vm_offset_t offset, size_t count)
{
	vm_object_t obj = vp->v_object;
	bool success;

	KASSERT(obj != NULL, ("vnode has no VM object"));

	VOP_LOCK(vp, LK_EXCLUSIVE);
	VM_OBJECT_WLOCK(obj);
	success = vm_object_populate(obj, offset, count);
	VM_OBJECT_WUNLOCK(obj);
	VOP_UNLOCK(vp, 0);

	if (!success)
		return (EFAULT);

	atomic_add_64(&sls_prefault_vnios, 1);
	atomic_add_64(&sls_prefault_vnpages, count);

	return (0);
}

static int
slspre_vnode_prefault(struct vnode *vp, struct sls_prefault *slspre)
{
	vm_object_t obj = vp->v_object;
	size_t offset, count, start;
	int error;

	offset = 0;
	for (;;) {
		/* Go over the blocks we don't need. */
		while (offset < obj->size) {
			if (bit_test(slspre->pre_map, offset) != 0)
				break;
			offset += 1;
		}

		start = offset;
		count = 0;
		while (offset < obj->size) {
			if (bit_test(slspre->pre_map, offset) == 0)
				break;

			count += 1;
			if (count >= MAXPRE_PAGES)
				break;
		}

		KASSERT(count <= MAXPRE_PAGES, ("pagein too large"));

		if (start == obj->size)
			break;

		error = slspre_getpages(vp, start, count);
		if (error != 0)
			return (error);

		offset += count;
	}

	return (0);
}

static int
slspre_vnode_eager(struct vnode *vp)
{
	size_t pages_left, pages;
	size_t offset;
	int error;

	for (offset = 0; offset < vp->v_object->size; offset += pages) {
		pages_left = vp->v_object->size - offset;
		pages = min(MAXPRE_PAGES, pages_left);

		error = slspre_getpages(vp, offset, pages);
		if (error != 0)
			return (error);

		if (pages_left <= MAXPRE_PAGES)
			return (slspre_getpages(vp, offset, pages));
	}

	return (0);
}

int
slspre_vnode(struct vnode *vp, struct sls_attr attr)
{
	uint64_t slsid = INUM(SLSVP(vp));
	struct sls_prefault *slspre;

	if (SLSATTR_ISPREFAULT(attr) || SLSATTR_ISDELTAREST(attr)) {
		if (slskv_find(
			slsm.slsm_prefault, slsid, (uintptr_t *)&slspre) != 0)
			return (0);

		return (slspre_vnode_prefault(vp, slspre));
	}

	if (SLSATTR_ISLAZYREST(attr))
		return (0);

	slspre_vnode_eager(vp);

	return (0);
}

int slspre_clear(SYSCTL_HANDLER_ARGS)
{
	struct sls_prefault *slspre;
	uint64_t prefaultid;
	const int done = 1;
	int error;

	KV_FOREACH_POP(slsm.slsm_prefault, prefaultid, slspre)
	slspre_destroy(slspre);

	error = SYSCTL_OUT(req, &done, sizeof(done));
	return (error);
}
