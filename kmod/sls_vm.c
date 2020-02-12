#include <sys/types.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/queue.h>

#include <sys/conf.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_vm.h"

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
	slsvm_object_shadowexact(objp);

	error = slskv_add(objtable, (uint64_t) obj, (uintptr_t) *objp);
	if (error != 0) {
	    vm_object_deallocate(*objp);
	    return (error);
	}

	return (0);
}

void
slsvm_objtable_collapse(struct slskv_table *objtable)
{
	struct slskv_iter iter;
	vm_object_t obj, shadow;
	
	/* Remove the Aurora - created shadow. */
	KV_FOREACH(objtable, iter, obj, shadow) 
	    vm_object_deallocate(obj);

	/* XXX Don't forget the SYSV shared memory table! */
}

/*
 * Destroy all physical process memory mappings for the entry. Only makes
 * sense for writable entries, read-only entries are retained.
 */
void
slsvm_entry_zap(struct proc *p, struct vm_map_entry *entry)
{
	/* We could do it for currently read only if we did it lazily */
	if ((entry->max_protection & VM_PROT_WRITE) == 0)
	    return;

	pmap_remove(&p->p_vmspace->vm_pmap, entry->start, entry->end);
}

/*
 * Below are the two functions used for creating the shadows, slsvm_procset_shadow()
 * and slsvm_objtable_collapse(). What slsvm_procset_shadow does it take a reference on the
 * objects that are directly attached to the process' entries, and then create
 * a shadow for it. The entry's reference to the original object is transferred
 * to the shadow, while the new object gets another reference to the original.
 * As a net result, if O the old object, S the shadow, and E the entry, we
 * go from 
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

/*
 * Get all objects accessible throught the VM entries of the checkpointed process.
 * This causes any CoW objects shared among memory maps to split into two. 
 */
int 
slsvm_proc_shadow(struct proc *p, struct slskv_table *table, int is_fullckpt)
{
	struct vm_map_entry *entry, *header;
	vm_object_t obj, vmshadow;
	int error;

	header = &p->p_vmspace->vm_map.header;
	for (entry = header->next; entry != header; entry = entry->next) {
		obj = entry->object.vm_object;
		/* 
		 * Non anonymous objects cannot shadowed meaningfully. Guard 
		 * entries are null.
		 */
		if (!OBJT_ISANONYMOUS(obj))
		    continue;

		/* 
		 * Check if we have already shadowed the object. If we did, 
		 * have the process map point to the shadow.
		 */
		if (slskv_find(table, (uint64_t) obj, (uintptr_t *) &vmshadow) == 0) {
		    entry->object.vm_object = vmshadow;
		    slsvm_object_reftransfer(obj, vmshadow);
		    slsvm_entry_zap(p, entry);
		    continue;
		}

		/* Shadow the object, retain it in Aurora. */
		error = slsvm_object_shadow(table, &entry->object.vm_object);
		if (error != 0)
		    goto error;

		slsvm_entry_zap(p, entry);

		if (!is_fullckpt)
		    continue;

		/* If in a full checkpoint, checkpoint down the tree. */

		obj = obj->backing_object;
		while (OBJT_ISANONYMOUS(obj)) {
		    vm_object_reference(obj);
		    /* These objects have no shadows. */
		    error = slskv_add(table, (uint64_t) obj, (uintptr_t) NULL);
		    if (error != 0) {
			/* Already in the table. */
			vm_object_deallocate(obj);
			break;
		    }

		    obj = obj->backing_object;
		}
	}

	return (0);

error:

	/* Deallocate only the shadows we have control over. */
	uma_zfree(slskv_zone, table);

	return (error);
}

/* Collapse the backing objects of all processes under checkpoint. */
int 
slsvm_procset_shadow(slsset *procset, struct slskv_table *table, int is_fullckpt) 
{
	struct slskv_iter iter;
	struct proc *p;
	int error;

	KVSET_FOREACH(procset, iter, p) {
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
	vm_object_reference(dst);
	vm_object_deallocate(src);
}

/* Create a shadow of the same size as the object, perfectly aligned. */
void
slsvm_object_shadowexact(vm_object_t *objp)
{
	vm_ooffset_t offset = 0;

	vm_object_shadow(objp, &offset, ptoa((*objp)->size));
}
