#include <sys/param.h>

#include <machine/param.h>

#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/sysent.h>
#include <sys/vnode.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "slsmm.h"
#include "sls_shadow.h"

vm_object_t *
sls_shadow(struct proc *p, size_t *numobjects)
{
	vm_map_t vm_map;
	struct vm_map_entry *entry;
	vm_object_t *objects;
	size_t arrsize;
	vm_object_t obj;
	int i;

	vm_map = &p->p_vmspace->vm_map;
	arrsize = vm_map->nentries;

	objects = malloc(sizeof(*objects) * arrsize, M_SLSMM, M_WAITOK);

	for (entry = vm_map->header.next, i = 0; entry != &vm_map->header;
		entry = entry->next, i++) {
	    /* We only save the pages of anonymous objects */
	    obj = entry->object.vm_object;
	    if (obj == NULL || obj->type != OBJT_DEFAULT) {
		objects[i] = NULL;
		continue;
	    }

	    objects[i] = obj;

	    /* Common code for shadow and delta dumps */
	    vm_object_reference(entry->object.vm_object);
	    vm_object_shadow(&entry->object.vm_object, &entry->offset,
		    entry->end-entry->start);

	    /* Always remove the mappings, so that the process rereads them */
	    pmap_remove(vm_map->pmap, entry->start, entry->end);
	}

	*numobjects = arrsize;
	return objects;
}

void
sls_compact(vm_object_t *objects, size_t numobjects)
{
	vm_object_t obj;
	int i;

	for (i = 0; i < numobjects; i++) {
	    obj = objects[i];
	    if (obj == NULL)
		continue;

	    vm_object_deallocate(obj->backing_object);
	}
}
