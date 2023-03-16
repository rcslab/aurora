#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitstring.h>
#include <sys/file.h>
#include <sys/md5.h>
#include <sys/proc.h>
#include <sys/uio.h>

#include <vm/vm_page.h>
#include <vm/vm_param.h>

#include "sls_internal.h"
#include "sls_io.h"
#include "sls_table.h"

/* Point at which we cut off the prefault vector when printing out. */
#define SLSPG_BUFSZ (1024)

static int
slspre_track_entries(vm_map_t map, char *buf, struct file *fp)
{
	vm_map_entry_t entry;
	vm_object_t obj;
	uint64_t objid;
	size_t len;
	int error;
	int type;

	/* Go through the process mappings. */
	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		obj = entry->object.vm_object;
		objid = (obj != NULL) ? obj->objid : 0UL;
		type = (obj != NULL) ? obj->type : 0;

		snprintf(buf, SLSPG_BUFSZ - 1, "E %lx %lx %lx %x %d\n",
		    entry->start, entry->end, objid, entry->protection, type);
		len = strnlen(buf, SLSPG_BUFSZ);

		error = slsio_fpwrite(fp, buf, len);
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slspre_track_vnodes(vm_map_t map, char *buf, struct file *fp)
{
	vm_map_entry_t entry;
	vm_object_t obj;
	int error;

	/* Go through the process mappings. */
	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		for (obj = entry->object.vm_object; obj != NULL;
		     obj = obj->backing_object) {
			if (obj->type == OBJT_VNODE)
				break;
		}

		if (obj == NULL)
			continue;

		KASSERT(
		    obj->type == OBJT_VNODE, ("object doesn't back a vnode"));
		snprintf(
		    buf, SLSPG_BUFSZ, "V %p %ld\n", obj->handle, obj->size);

		error = slsio_fpwrite(fp, buf, strnlen(buf, SLSPG_BUFSZ));
		if (error != 0) {
			free(buf, M_SLSMM);
			return (error);
		}
	}

	return (0);
}

static void
md5_page(vm_page_t m, char digest[MD5_DIGEST_LENGTH])
{
	MD5_CTX md5sum;

	vm_page_assert_locked(m);

	MD5Init(&md5sum);
	MD5Update(&md5sum, (void *)PHYS_TO_DMAP(m->phys_addr), PAGE_SIZE);
	MD5Final(digest, &md5sum);
}

static int
slspre_track_pages(vm_map_t map, pmap_t pmap, char *buf, struct file *fp)
{
	char digest[MD5_DIGEST_LENGTH];
	vm_map_entry_t entry;
	vm_offset_t addr;
	vm_paddr_t pa;
	vm_page_t m;
	bool hascontents;
	size_t len;
	int error;

	/* Extract the page table maps themselves. */
	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		for (addr = entry->start; addr < entry->end;
		     addr += PAGE_SIZE) {
			pa = pmap_extract(pmap, addr);
			if (pa == 0ULL)
				continue;

			m = PHYS_TO_VM_PAGE(pa);
			KASSERT(m != NULL,
			    ("non NULL physical address has NULL page"));

			vm_page_lock(m);
			vm_page_hold(m);

			/* Edge case for the VDSO page. */
			if (m->oflags & VPO_UNMANAGED) {
				vm_page_unhold(m);
				vm_page_unlock(m);
				continue;
			}

			/* Edge case for the timer device mapping. */
			if (m->object == NULL) {
				vm_page_unhold(m);
				vm_page_unlock(m);
				continue;
			}

			md5_page(m, digest);

			VM_OBJECT_WLOCK(m->object);
			_Static_assert(
			    2 * sizeof(uint64_t) == MD5_DIGEST_LENGTH,
			    "unexpected MD5 digest length");

			hascontents = memcmp(
			    zero_region, (void *)PHYS_TO_DMAP(pa), PAGE_SIZE);
			snprintf(buf, SLSPG_BUFSZ, "P %lx %d %d %lx%lx %c\n",
			    addr, pmap_is_modified(m) ? 1 : 0,
			    pmap_ts_referenced(m), *(uint64_t *)digest,
			    *(uint64_t *)&digest[sizeof(uint64_t)],
			    hascontents ? 'N' : 'Z');
			len = strnlen(buf, SLSPG_BUFSZ);

			pmap_clear_modify(m);
			if (pmap_is_modified(m))
				panic("Did not clear modify bit");

			VM_OBJECT_WUNLOCK(m->object);
			vm_page_unhold(m);
			vm_page_unlock(m);

			error = slsio_fpwrite(fp, buf, len);
			if (error != 0) {
				free(buf, M_SLSMM);
				return (error);
			}
		}
	}

	return (0);
}

static int
slspre_track_map(struct vmspace *vm, struct file *fp)
{
	int error;
	char *buf;

	buf = malloc(SLSPG_BUFSZ, M_SLSMM, M_WAITOK);

	error = slspre_track_vnodes(&vm->vm_map, buf, fp);
	if (error != 0)
		goto done;

	error = slspre_track_entries(&vm->vm_map, buf, fp);
	if (error != 0)
		goto done;

	error = slspre_track_pages(&vm->vm_map, &vm->vm_pmap, buf, fp);
	if (error != 0)
		goto done;

	/* Blow away the mappings. */
	pmap_invalidate_all(&vm->vm_pmap);
	pmap_remove(&vm->vm_pmap, 0, VM_MAXUSER_ADDRESS);
	pmap_invalidate_all(&vm->vm_pmap);

done:

	free(buf, M_SLSMM);
	return (0);
}

int
slspre_resident(struct slspart *slsp, struct file *fp)
{
	struct proc *pcaller = curproc;
	struct slskv_iter iter;
	slsset *procset;
	struct proc *p;
	int stateerr;
	int error;

	/* The set of processes we are going to checkpoint. */
	if (!slsckpt_prepare_state(slsp, NULL)) {
		return (EBUSY);
	}

	/* The set of processes we are going to checkpoint. */
	error = slsset_create(&procset);
	if (error != 0)
		goto done;

	error = slsckpt_gather(slsp, procset, pcaller, false);
	if (error != 0) {
		slsset_destroy(procset);
		goto done;
	}

	slsckpt_stop(procset, pcaller);
	KVSET_FOREACH(procset, iter, p)
	{
		error = slspre_track_map(p->p_vmspace, fp);
		if (error != 0) {
			KV_ABORT(iter);
			break;
		}
	}
	slsckpt_cont(procset, pcaller);

	KVSET_FOREACH_POP(procset, p)
	PRELE(p);

	slsset_destroy(procset);

done:
	stateerr = slsp_setstate(
	    slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("partition in state %d", slsp->slsp_status));

	return (error);
}
