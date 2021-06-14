#include <sys/types.h>
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/rwlock.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_param.h>
#include <vm/vm_radix.h>

#include <machine/pmap.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_pager.h"
#include "sls_vm.h"

int sls_objprotect = 1;
SDT_PROBE_DEFINE(sls, , , procset_loop);

/*
 * Create a shadow of the same size as the object, perfectly aligned.
 */
static void
slsvm_object_shadowexact(vm_object_t *objp)
{
	vm_ooffset_t offset = 0;
	vm_object_t obj;

	obj = *objp;
#ifdef VERBOSE
	DEBUG2("(PRE) Object %p has %d references", obj, obj->ref_count);
#endif
	vm_object_shadow(objp, &offset, ptoa((*objp)->size));

	KASSERT((obj != *objp), ("object %p wasn't shadowed", obj));
#ifdef VERBOSE
	DEBUG2("(POST) Object %p has %d references", obj, obj->ref_count);
	DEBUG2("(SHADOW) Object %p has shadow %p", obj, *objp);
#endif
	/* Inherit the unique object ID from the parent. */
	VM_OBJECT_WLOCK(*objp);
	DEBUG3("Changing the ID of %p from %lx to %lx", *objp, (*objp)->objid,
	    obj->objid);
	(*objp)->objid = obj->objid;
	VM_OBJECT_WUNLOCK(*objp);
}

/*
 * Shadow an object. Take an extra reference on behalf of Aurora for the
 * object, and transfer the one already in the object to the shadow. The
 * transfer follows the following logic: The object has a reference because
 * some memory location in some remote data structure holds a pointer to it.
 * Pass that exact location to the vm_object_shadow() function to replace the
 * pointer with one to the new object.
 *
 * Save the object-shadow pair in the table.
 */
int
slsvm_object_shadow(struct slskv_table *objtable, vm_object_t *objp)
{
	vm_object_t obj;
	int error;

	obj = *objp;
	vm_object_reference(obj);
	/*
	 * If we are entering a swap object in Aurora, we need to set up its
	 * swap space.
	 */
	if (obj->type == OBJT_SWAP) {
		VM_OBJECT_WLOCK(obj);
		error = sls_pager_obj_init(obj);
		VM_OBJECT_WUNLOCK(obj);
		if (error != 0) {
			vm_object_deallocate(obj);
			return (error);
		}
	}
	slsvm_object_shadowexact(objp);
	obj->flags |= OBJ_AURORA;

	/*
	 * Shadow objects aren't actually in Aurora! They are directly used
	 * by processes, so we cannot modify them or dump them.
	 */
	KASSERT(((*objp)->flags & OBJ_AURORA) == 0, ("shadow is in Aurora"));
	DEBUG2("Shadow pair (%p, %p)", obj, *objp);
	error = slskv_add(objtable, (uint64_t)obj, (uintptr_t)*objp);
	if (error != 0) {
		vm_object_deallocate(*objp);
		return (error);
	}

	return (0);
}

/*
 * The same as vm_object_shadow, with different refcount handling and return
 * values. We also always create a shadow, regardless of the refcount.
 */
void
slsvm_forceshadow(vm_object_t shadow, vm_object_t source, vm_ooffset_t offset)
{
	/*
	 * Store the offset into the source object, and fix up the offset into
	 * the new object.
	 */
	shadow->backing_object = source;
	shadow->backing_object_offset = offset;

	VM_OBJECT_WLOCK(source);
	shadow->domain = source->domain;
	LIST_INSERT_HEAD(&source->shadow_head, shadow, shadow_list);
	source->shadow_count++;

#if VM_NRESERVLEVEL > 0
	shadow->flags |= source->flags & OBJ_COLORED;
	shadow->pg_color = (source->pg_color + OFF_TO_IDX(offset)) &
	    ((1 << (VM_NFREEORDER - 1)) - 1);
#endif
	VM_OBJECT_WUNLOCK(source);
}

/*
 * Collapse the new table into the old one.
 */
void
slsvm_objtable_collapsenew(
    struct slskv_table *objtable, struct slskv_table *newtable)
{
	vm_object_t ancestor, ashadow, shadow, child;
	int error;

	/* We can have multiple objects (e.g. because of forks). */
	KV_FOREACH_POP(newtable, shadow, child)
	{
		VM_OBJECT_RLOCK(shadow);
		/*
		 * If the child is NULL, we didn't really shadow - this is an
		 * internal object we want to hold. Push it all to the table.
		 */

		/*
		 * Find if there is an ancestor of us that is in the table.
		 * It is not necessarily our parent that is there, because it
		 * might not yet be deallocated, e.g., because it is being
		 * dumped.
		 */
		for (ancestor = shadow->backing_object; ancestor != NULL;
		     ancestor = ancestor->backing_object) {
			error = slskv_find(objtable, (uint64_t)ancestor,
			    (uintptr_t *)&ashadow);
			if (error != 0)
				continue;

			/* Replace the shadow with the new child.  */
			KASSERT(
			    child != NULL, ("found shadow with no children"));
			slskv_del(objtable, (uint64_t)ancestor);
			VM_OBJECT_RUNLOCK(shadow);

			error = slskv_add(
			    objtable, (uint64_t)ancestor, (uintptr_t)child);
			KASSERT(error == 0,
			    ("Reinserting parent into table failed"));
			vm_object_deallocate(shadow);

			break;
		}

		/* If we found an ancestor, we fixed this object. */
		if (ancestor != NULL)
			continue;

		/* Object not in the table, add it. */
		VM_OBJECT_RUNLOCK(shadow);
		/* Move the shadow (and its reference) to the main table. */

		error = slskv_add(objtable, (uint64_t)shadow, (uintptr_t)child);
		KASSERT(error == 0, ("failed to insert new shadow"));
	}
}

/*
 * Collapse the old object table into the new one.
 */
void
slsvm_objtable_collapse(
    struct slskv_table *objtable, struct slskv_table *newtable)
{
	vm_object_t obj, shadow, child;
	int error;

	/* Remove the Aurora reference from the backing objects. */
	KV_FOREACH_POP(objtable, obj, shadow)
	{
		KASSERT(obj != NULL, ("Null object"));

		DEBUG4(
		    "Deallocating object %p (ID %lx), with %d references and %d shadows",
		    obj, obj->objid, obj->ref_count, obj->shadow_count);
		if (newtable == NULL) {
			vm_object_deallocate(obj);
			continue;
		}

		/* Collapse the old shadow into the parent. */
		error = slskv_find(
		    newtable, (uint64_t)shadow, (uintptr_t *)&child);
		if (error != 0) {
			/* Shadow not in the checkpoint, destroy the original
			 * object. */
			vm_object_deallocate(obj);
			continue;
		}

		/* The shadow is still in the checkpoint. */
		vm_object_deallocate(shadow);
		slskv_del(newtable, (uint64_t)shadow);
		error = slskv_add(newtable, (uint64_t)obj, (uintptr_t)child);
		KASSERT(error == 0,
		    ("Object %p shadowed in consecutive checkpoints", obj));
	}
}

/*
 * Destroy all physical process memory mappings for the entry. Only makes
 * sense for writable entries, read-only entries are retained.
 */
static void
slsvm_entry_protect_obj(struct proc *p, struct vm_map_entry *entry)
{
	if (((entry->eflags & MAP_ENTRY_NEEDS_COPY) == 0) &&
	    ((entry->protection & VM_PROT_WRITE) != 0)) {
		pmap_protect_pglist(&p->p_vmspace->vm_pmap, entry->start,
		    entry->end, entry->start - entry->offset,
		    &entry->object.vm_object->memq,
		    entry->protection & ~VM_PROT_WRITE, AURORA_PMAP_TEST_FULL);
	}
}

static void
slsvm_entry_protect(struct proc *p, struct vm_map_entry *entry)
{
	KASSERT(
	    entry->wired_count == 0, ("wired count is %d", entry->wired_count));

	if (((entry->eflags & MAP_ENTRY_NEEDS_COPY) == 0) &&
	    ((entry->protection & VM_PROT_WRITE) != 0)) {
		pmap_protect(&p->p_vmspace->vm_pmap, entry->start, entry->end,
		    entry->protection & ~VM_PROT_WRITE);
	}
}

void
slsvm_object_copy(struct proc *p, struct vm_map_entry *entry, vm_object_t obj)
{
	vm_page_t cur, copy, original, tmp;
	vm_offset_t vaddr;

	SLS_DBG("(%p) Object copy start\n", obj);

	KASSERT(obj->type != OBJT_DEAD, ("object %p is dead", obj));
	KASSERT(obj->backing_object != NULL, ("object %p is unbacked", obj));
	KASSERT(obj->backing_object->type != OBJT_DEAD,
	    ("object %p is dead", obj->backing_object));

	/* Lock the object before locking its backer, else we can deadlock. */
	VM_OBJECT_WLOCK(obj);
	VM_OBJECT_WLOCK(obj->backing_object);

	KASSERT(obj->type != OBJT_DEAD, ("object %p is dead", obj));
	KASSERT(obj->backing_object != NULL, ("object %p is unbacked", obj));
	KASSERT(obj->backing_object->type != OBJT_DEAD,
	    ("object %p is dead", obj->backing_object));

	TAILQ_FOREACH_SAFE (cur, &obj->backing_object->memq, listq, tmp) {
		/* Check if already copied. */
		original = vm_page_lookup(obj, cur->pindex);
		if (original != NULL)
			continue;

		/* Otherwise create a blank copy, and fix up the page tables. */
		copy = vm_page_grab(
		    obj, cur->pindex, VM_ALLOC_NORMAL | VM_ALLOC_NOBUSY);
		/* Copy the data into the page and mark it as valid. */
		pmap_copy_page(cur, copy);
		copy->valid = VM_PAGE_BITS_ALL;

		vaddr = entry->start - entry->offset + IDX_TO_OFF(cur->pindex);
		pmap_enter(&p->p_vmspace->vm_pmap, vaddr, copy,
		    entry->protection, VM_PROT_WRITE | VM_PROT_COPY, 0);
	}
	VM_OBJECT_WUNLOCK(obj->backing_object);
	VM_OBJECT_WUNLOCK(obj);

	SLS_DBG("(%p) Object copy end\n", obj);
}

/*
 * Below are the two functions used for creating the shadows,
 * slsvm_procset_shadow() and slsvm_objtable_collapse(). What
 * slsvm_procset_shadow does it take a reference on the objects that are
 * directly attached to the process' entries, and then create a shadow for it.
 * The entry's reference to the original object is transferred to the shadow,
 * while the new object gets another reference to the original. As a net result,
 * if O the old object, S the shadow, and E the entry, we go from
 *
 * [O (1)] - E
 *
 * to
 *
 * [O (2)] - [S (1)] - E
 *    |
 *   SLS
 *
 * where the numbers in parentheses are the number of references and lines
 * denote a reference to objects. To collapse the objects, we remove the
 * reference taken by the SLS.
 */
int
slsvm_entry_shadow(struct proc *p, struct slskv_table *table,
    vm_map_entry_t entry, bool is_fullckpt)
{
	vm_object_t obj, vmshadow;
	int error;

	obj = entry->object.vm_object;
	/*
	 * Non anonymous objects cannot shadowed meaningfully.
	 * Guard entries are null.
	 */
	if (!OBJT_ISANONYMOUS(obj))
		return (0);

	/*
	 * Check if we have already shadowed the object. If we did,
	 * have the process map point to the shadow.
	 */
	if (slskv_find(table, (uint64_t)obj, (uintptr_t *)&vmshadow) == 0) {
		KASSERT((obj->flags & OBJ_AURORA) != 0,
		    ("object %p in table not in Aurora", obj));

		/*
		 * If an object is already in the table, and it has no
		 * shadow, it means it was the ancestor of an object attached to
		 * a VM entry. That means it had a shadow.  Objects with shadows
		 * _cannot_ be directly linked, since that would mean that the
		 * shadow would either see or not see the writes, depending on
		 * whether it already had a private copy (the kernel asserts for
		 * this in some places). If the object is writable but has a
		 * shadow, then the entry _has_ to be marked CoW, so that the
		 * fault handler fixes up the entry after forking.
		 */
		if (vmshadow == NULL) {
			KASSERT((obj->shadow_count == 0) ||
				((entry->protection & VM_PROT_WRITE) == 0) ||
				((entry->eflags & MAP_ENTRY_COW) != 0),
			    ("directly accessible writable object %p has %d shadows",
				obj, obj->shadow_count));

			/*
			 * In all these cases, we don't actually need to do
			 * anything; the object cannot be written to, and will
			 * be safely shadowed by the system if needed.
			 */

			return (0);
		}

		/*
		 * There is no race with the process here for the
		 * entry, so there is no need to lock the map.
		 */
		if (sls_objprotect == 1)
			slsvm_entry_protect_obj(p, entry);
		else
			slsvm_entry_protect(p, entry);
		entry->object.vm_object = vmshadow;
		VM_OBJECT_WLOCK(vmshadow);
		vm_object_clear_flag(vmshadow, OBJ_ONEMAPPING);
		slsvm_object_reftransfer(obj, vmshadow);
		VM_OBJECT_WUNLOCK(vmshadow);
		return (0);
	}

	/* Shadow the object, retain it in Aurora. */
	if (sls_objprotect == 1)
		slsvm_entry_protect_obj(p, entry);
	else
		slsvm_entry_protect(p, entry);
	error = slsvm_object_shadow(table, &entry->object.vm_object);
	if (error != 0)
		return (error);

	/*
	 * We use is_fullckpt for the sole reason of taking numbers for full
	 * checkpoints.  Full checkpoints make zero sense as a concept, since we
	 * have the guarantee that objects that have been dumped once have been
	 * unmodified. Checkpointing an object with the OBJ_AURORA flag for any
	 * reason other than comparing naive and delta checkpoints is just
	 * stupid.
	 */

	/* Checkpoint down the tree. */
	obj = obj->backing_object;
	while (OBJT_ISANONYMOUS(obj) &&
	    (is_fullckpt || ((obj->flags & OBJ_AURORA) == 0))) {
		vm_object_reference(obj);
		obj->flags |= OBJ_AURORA;
		DEBUG2("Shadow pair (%p, %p)", obj, NULL);
		/* These objects have no shadows. */
		error = slskv_add(table, (uint64_t)obj, (uintptr_t)NULL);
		if (error != 0) {
			/* Already in the table. */
			vm_object_deallocate(obj);
			break;
		}

		obj = obj->backing_object;
	}

	return (0);
}

/*
 * Get all objects accessible through the VM entries of the checkpointed
 * process. This causes any CoW objects shared among memory maps to split into
 * two.
 */
static int
slsvm_proc_shadow(struct proc *p, struct slskv_table *table, bool is_fullckpt)
{
	struct vm_map_entry *entry, *header;
	int error;

	header = &p->p_vmspace->vm_map.header;
	for (entry = header->next; entry != header; entry = entry->next) {
		error = slsvm_entry_shadow(p, table, entry, is_fullckpt);
		if (error != 0)
			return (error);
	}

	return (0);
}

/* Collapse the backing objects of all processes under checkpoint. */
int
slsvm_procset_shadow(
    slsset *procset, struct slskv_table *table, bool is_fullckpt)
{
	struct slskv_iter iter;
	struct proc *p;
	int error;

	KVSET_FOREACH(procset, iter, p)
	{
		SDT_PROBE0(sls, , , procset_loop);
		error = slsvm_proc_shadow(p, table, is_fullckpt);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}
	}

	return (0);
}
/* Transfer a reference between objects. */
void
slsvm_object_reftransfer(vm_object_t src, vm_object_t dst)
{
	VM_OBJECT_ASSERT_WLOCKED(dst);
	vm_object_reference_locked(dst);
	vm_object_deallocate(src);
}

/*
 * Print vmobject to KTR
 */
void
slsvm_print_vmobject(struct vm_object *obj)
{
	vm_object_t current_object = obj;

	do {
		DEBUG2("    vm_object:%p offset:%llx", current_object,
		    current_object->backing_object_offset);
		DEBUG2("                 type:%x flags:%x",
		    current_object->type, current_object->flags);
		DEBUG1("    	objid:%lx", current_object->objid);
	} while ((current_object = current_object->backing_object) != NULL);
}

/*
 * Print vmspace to KTR
 */
void
slsvm_print_vmspace(struct vmspace *vm)
{
	vm_map_t map = &vm->vm_map;
	vm_map_entry_t entry;
	vm_object_t obj;

	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		KASSERT((entry->eflags & MAP_ENTRY_IS_SUB_MAP) == 0,
		    ("entry is submap"));
		DEBUG3("vm_entry:%p range:%llx--%llx", entry, entry->start,
		    entry->end);
		DEBUG2("            eflags:%x inheritance:%x", entry->eflags,
		    entry->inheritance);
		DEBUG2("            offset:%x protection:%x", entry->offset,
		    entry->protection);
		obj = entry->object.vm_object;
		if (obj != NULL) {
			slsvm_print_vmobject(obj);
		} else {
			DEBUG("            NULL object");
		}
	}
}

/*
 * Write out the start of every page accessible from an address space, and a
 * CRC32 hash of all its contests.
 */
void
slsvm_print_crc32_vmspace(struct vmspace *vm)
{
	vm_map_t map = &vm->vm_map;
	vm_map_entry_t entry;
	vm_object_t obj;
	vm_page_t m;
	vm_offset_t addr;

	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		KASSERT((entry->eflags & MAP_ENTRY_IS_SUB_MAP) == 0,
		    ("entry is submap"));
		obj = entry->object.vm_object;
		DEBUG4("entry: 0x%lx\tpresent: %s obj %p pages %ld",
		    entry->start, (obj != NULL) ? "yes" : "no", obj,
		    (obj != NULL) ? obj->resident_page_count : 0);
		if (!OBJT_ISANONYMOUS(obj))
			continue;
		TAILQ_FOREACH (m, &obj->memq, listq) {
			addr = IDX_TO_OFF(m->pindex) - entry->offset +
			    entry->start;
			DEBUG3("0x%lx, 0x%x 0x%x", addr,
			    *(uint64_t *)PHYS_TO_DMAP(m->phys_addr),
			    *((uint64_t *)PHYS_TO_DMAP(m->phys_addr) + 1));
		}
	}
}

/*
 * Write out the start of every page accessible from an address space, and a
 * CRC32 hash of all its contests.
 */
void
slsvm_print_crc32_object(vm_object_t obj)
{
	vm_page_t m;

	if (!OBJT_ISANONYMOUS(obj))
		return;

	TAILQ_FOREACH (m, &obj->memq, listq) {
		DEBUG4("(%p) 0x%lx, 0x%x 0x%x", obj, IDX_TO_OFF(m->pindex),
		    *(uint64_t *)PHYS_TO_DMAP(m->phys_addr),
		    *((uint64_t *)PHYS_TO_DMAP(m->phys_addr) + 1));
	}
}

void
slsvm_object_scan(void)
{
	vm_object_t obj;

	mtx_lock(&vm_object_list_mtx);
	TAILQ_FOREACH (obj, &vm_object_list, object_list) {
		if ((obj->flags & OBJ_AURORA) == 0)
			continue;

		mtx_unlock(&vm_object_list_mtx);
		VM_OBJECT_WLOCK(obj);
		if ((obj->flags & OBJ_DEAD) != 0)
			goto next_object;

		atomic_thread_fence_acq();
		if ((obj->flags & OBJ_AURORA) == 0)
			goto next_object;

		printf("Object %p has %d refs\n", obj, obj->ref_count);
	next_object:
		VM_OBJECT_WUNLOCK(obj);
		mtx_lock(&vm_object_list_mtx);
	}
	mtx_unlock(&vm_object_list_mtx);
	printf("---------\n");
}

void
slsvm_pages_dump(struct vmspace *vm, struct slskv_table *table)
{
	vm_map_entry_t entry;
	vm_page_t m;
	vm_object_t obj;
	vm_ooffset_t addr, last = 0;
	vm_map_t map = &vm->vm_map;
	int error;
	uintptr_t value;

	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		obj = entry->object.vm_object;
		if (!OBJT_ISANONYMOUS(obj))
			continue;
		TAILQ_FOREACH (m, &obj->memq, listq) {
			addr = IDX_TO_OFF(m->pindex) - entry->offset +
			    entry->start;

			if (addr <= last)
				break;
			last = addr;
			value = *((uint64_t *)PHYS_TO_DMAP(m->phys_addr) + 64);
			error = slskv_add(table, addr, value);
			if (error != 0)
				printf("Inserted page %lx twice\n", addr);
		}
	}
}

void
slsvm_pages_check(struct vmspace *vm, struct slskv_table *table)
{
	vm_map_entry_t entry;
	vm_page_t m;
	vm_object_t obj;
	vm_ooffset_t addr, last;
	vm_map_t map = &vm->vm_map;
	uintptr_t expected, val;
	int error;

	for (entry = map->header.next; entry != &map->header;
	     entry = entry->next) {
		obj = entry->object.vm_object;
		if (!OBJT_ISANONYMOUS(obj))
			continue;
		TAILQ_FOREACH (m, &obj->memq, listq) {
			addr = IDX_TO_OFF(m->pindex) - entry->offset +
			    entry->start;
			if (addr <= last)
				break;
			last = addr;
			error = slskv_find(table, addr, &val);
			if (error != 0) {
				printf("Didn't find page %lx\n", addr);
				continue;
			}
			expected = *(
			    (uint64_t *)PHYS_TO_DMAP(m->phys_addr) + 64);
			if (expected != val)
				printf(
				    "ERROR: Address %lx should have had %lx, has %lx\n",
				    addr, val, expected);
			slskv_del(table, addr);
		}
	}
}
