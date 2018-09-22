#include <sys/types.h>

#include <sys/limits.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/shm.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <machine/param.h>

#include "memckpt.h"
#include "_slsmm.h"
#include "slsmm.h"
#include "fileio.h"

/* insert a shadow object to each vm_object in a vmspace,
 * return the list of original vm_object and vm_entry for dump
 */
int
vmspace_checkpoint(struct vmspace *vmspace, vm_object_t *objects,
		struct vm_map_entry_info *entries, int mode)
{
	int error = 0;
	int i = 0;
	vm_map_t vm_map = &vmspace->vm_map;
	vm_map_entry_t entry;
	vm_pindex_t object_size;

	for (entry = vm_map->header.next, i = 0; entry != &vm_map->header; 
	     entry = entry->next, i++) {

		/* grab vm_object */
		objects[i] = entry->object.vm_object;
		if (objects[i]) object_size = objects[i]->size;
		else object_size = ULONG_MAX;

		/* grab vm_entry info */
		entries[i].start = entry->start;
		entries[i].end = entry->end;
		entries[i].offset = entry->offset;
		entries[i].eflags = entry->eflags;
		entries[i].protection = entry->protection;
		entries[i].max_protection = entry->max_protection;
		entries[i].size = object_size;
		entries[i].magic = SLS_ENTRY_INFO_MAGIC;

		/* insert a shadow object */
		if (mode == SLSMM_CKPT_DELTA) {
			vm_object_reference(entry->object.vm_object);
			vm_object_shadow(&entry->object.vm_object, &entry->offset,
					 entry->end-entry->start);
			pmap_remove(vm_map->pmap, entry->start, entry->end);
		}
	}

	return error;
}

/* give a list of objects and dump it to a given fd */
int
vmspace_dump(struct vmspace *vmspace, vm_object_t *objects,
		 struct vm_map_entry_info *entries, int fd, int mode) 
{
	int error = 0;
	vm_page_t page;
	vm_pindex_t poffset;
	vm_map_t vm_map = &vmspace->vm_map;
	vm_offset_t vaddr;
	struct vmspace_info vmspace_info = (struct vmspace_info) {
		.magic = SLS_VMSPACE_INFO_MAGIC,
		.vm_swrss = vmspace->vm_swrss,
		.vm_tsize = vmspace->vm_tsize,
		.vm_dsize = vmspace->vm_dsize,
		.vm_ssize = vmspace->vm_ssize,
		.vm_taddr = vmspace->vm_taddr,
		.vm_daddr = vmspace->vm_daddr,
		.vm_maxsaddr = vmspace->vm_maxsaddr,
		.nentries = vm_map->nentries,
	};

	error = fd_write(&vmspace_info, sizeof(struct vmspace_info), fd);
	if (error)
		return error;

	for (size_t i = 0; i < vm_map->nentries; i ++) {

		if (entries->eflags & MAP_ENTRY_IS_SUB_MAP) {
			printf("WARNING: Submap entry found, dump will be wrong\n");
			continue;
		}

		error = fd_write(entries+i, sizeof(struct vm_map_entry_info), fd);
		if (error)
			return error;
		if (objects[i] == NULL)
			continue;

		TAILQ_FOREACH(page, &objects[i]->memq, listq) {
			if (!page) {
				printf("ERROR: vm_page_t page is NULL");
				continue;
			}

			error = fd_write(&page->pindex, sizeof(vm_pindex_t), fd);
			if (error)
				return error;

            		vaddr = pmap_map(NULL, page->phys_addr, 
               				page->phys_addr+PAGE_SIZE, VM_PROT_READ);
			if (!vaddr)
				return error;

			error = fd_write((void*)vaddr, PAGE_SIZE, fd);
			if (error)
				return error;

        	}
		poffset = ULONG_MAX;
		fd_write(&poffset, sizeof(vm_pindex_t), fd);

		if (mode == SLSMM_CKPT_DELTA) 
			vm_object_deallocate(objects[i]);
	}

	return error;
}

int
vmspace_restore(struct proc *p, struct dump *dump)
{
	int error = 0;
	struct vmspace *vmspace = p->p_vmspace;
	struct vm_map *map = &vmspace->vm_map;
	vm_page_t page;

	/*
	 * XXX Reading should be done all at once, before starting restore,
	 * so that we are sure we have succeeded in reading all past state
	 * before going ahead with overwriting the process address space.
	 * Should be taken care of after making sure everything works. The
	 * same holds for the construction of the new vm_map.
	 */

	/*
	 * Blow away the old address space, as done in exec_new_vmspace
	 */

	pmap_remove_pages(vmspace_pmap(vmspace));
	vm_map_remove(map, vm_map_min(map), vm_map_max(map));

	/* XXX We have to look further into how to handle System V shmem */
	shmexit(vmspace);

	/* If all pages were wired using mlockall(), undo that */
	/* XXX Commented out because for some reason FreeBSD can't find vm_map_modflags */
	/*
	vm_map_lock(map);
	vm_map_modflags(map, 0, MAP_WIREFUTURE); 
	vm_map_unlock(map);
	*/

	/* Copy vmspace state to the existing vmspace */
	vmspace->vm_swrss = dump->vmspace.vm_swrss;
	vmspace->vm_tsize = dump->vmspace.vm_tsize;
	vmspace->vm_dsize = dump->vmspace.vm_dsize;
	vmspace->vm_ssize = dump->vmspace.vm_ssize;
	vmspace->vm_taddr = dump->vmspace.vm_taddr;
	vmspace->vm_taddr = dump->vmspace.vm_daddr;
	vmspace->vm_maxsaddr = dump->vmspace.vm_maxsaddr;

	/* restore vm_map entries */
	for (int i = 0; i < dump->vmspace.nentries; i ++) {
		if (dump->entries[i].size == ULONG_MAX)
			continue;

		VM_OBJECT_WLOCK(dump->objects[i]);
		TAILQ_FOREACH(page, &dump->objects[i]->memq, listq) {
			vm_page_lock(page);
			vm_pindex_t va = dump->entries[i].start + IDX_TO_OFF(page->pindex) - 
				dump->entries[i].offset;
			pmap_enter(vmspace_pmap(vmspace), va, page, 
					dump->entries[i].protection, VM_PROT_READ, 0);
			/* 
			 * Mark page as about to be used, keeping it from paging out. 
			 * Probably doesn't do anything, but maybe... (It should be removed 
			 * after we find the bug)
			 */
			vm_page_activate(page);
			vm_page_unlock(page);
		}
		VM_OBJECT_WUNLOCK(dump->objects[i]);

		vm_map_lock(map);
		error = vm_map_insert(map, dump->objects[i], dump->entries[i].offset, 
				dump->entries[i].start, dump->entries[i].end, 
				dump->entries[i].protection, dump->entries[i].max_protection, 
				MAP_COPY_ON_WRITE | MAP_PREFAULT);
		vm_map_unlock(map);

		if (error) {
			printf("vm_map_insert error\n");
			return error;
		}
	}
	
	return error;
}
