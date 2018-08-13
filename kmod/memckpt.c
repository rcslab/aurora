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

/*
 * XXX: We should check whether we dump all necessary info into the structures
 */
/*
 * Dump the address space into a file.
 */
int
vmspace_checkpoint(struct vmspace *vmspace, int fd)
{
	int error = 0;
	struct vm_object *vm_object;
	struct vm_map_entry *entry;
	struct vm_map_entry_info entry_info;
	struct vm_map *vm_map;
	vm_offset_t vaddr;
	vm_page_t page;
	vm_pindex_t object_size;
	vm_pindex_t poffset;
	struct vmspace_info vmspace_info;

	/* Dump the vmspace information, as well as the entry-node pairs */
	vm_map = &(vmspace->vm_map);
	vmspace_info = (struct vmspace_info) {
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

	/*
	 * Iteration pattern (start from -> next, stop at header) from
	 * FreeBSD code
	 */
	for (entry = vm_map->header.next; entry != &(vm_map->header);
	     entry = entry->next) {

		/*
		 * XXX: Cannot handle submaps yet, we suppose the .vm_object
		 * member is an object
		 */
		if (entry->eflags & MAP_ENTRY_IS_SUB_MAP) {
			printf("WARNING: Submap entry found, dump will be wrong\n");
			continue;
		}

		/*
		 * XXX: What does having a null vm object for the entry mean?
		 * (Probably that it is currently unused, so no mapping has
		 * been done)
		 */
		vm_object = entry->object.vm_object;
		if (vm_object != NULL) {
			object_size = vm_object->size;
		} else {
			/*
			 * Sentinel value, would need the whole space to be backed by
			 * a single object (possible but improbable).
			 */
			object_size = ULONG_MAX;
		}

		entry_info = (struct vm_map_entry_info) {
			.start = entry->start,
			.end = entry->end,
			.offset = entry->offset,
			.eflags = entry->eflags,
			.protection = entry->protection,
			.max_protection = entry->max_protection,
			.size = object_size,
		};
		error = fd_write(&entry_info, sizeof(struct vm_map_entry_info), fd);

		if (vm_object == NULL)
			continue;

		/*
		 * Dump the actual page data to disk after mapping it to
		 * the kernel
		 */
		TAILQ_FOREACH(page, &vm_object->memq, listq) {

			if (!page) {
				printf("ERROR: vm_page_t page is NULL");
				break;
			}
			/* The only thing we need from a page is its current virtual address */
			error = fd_write(&page->pindex, sizeof(vm_pindex_t), fd);
			if (error)
				break;
		
			vaddr = pmap_map(NULL, page->phys_addr,
				 page->phys_addr + PAGE_SIZE, VM_PROT_READ);

			if (!vaddr) {
				printf("ERROR: vaddr is NULL");
				break;
			}
			error = fd_write((void *)vaddr, PAGE_SIZE, fd);


			if (error)
				break;
		}

		/* Sentinel value, no more pages to read */
		poffset = ULONG_MAX;
		error = fd_write(&poffset, sizeof(vm_pindex_t), fd);
	}
	

	return error;
}

int
vmspace_restore(struct proc *p, int fd)
{
	struct vmspace_info vmspace_info;
	struct vm_map_entry_info entry_info;
	vm_pindex_t poffset;

	struct vmspace *vmspace;
	struct vm_map *map;
	struct vm_object *object;
	struct vm_page *page;
	vm_offset_t vaddr;

	int error = 0;

	vmspace = p->p_vmspace;
	map = &vmspace->vm_map;

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

	error = fd_read(&vmspace_info, sizeof(struct vmspace_info), fd);
	if (error)
		return error;

	/* Copy vmspace state to the existing vmspace */
	vmspace->vm_swrss = vmspace_info.vm_swrss;
	vmspace->vm_tsize = vmspace_info.vm_tsize;
	vmspace->vm_dsize = vmspace_info.vm_dsize;
	vmspace->vm_ssize = vmspace_info.vm_ssize;
	vmspace->vm_taddr = vmspace_info.vm_taddr;
	vmspace->vm_taddr = vmspace_info.vm_daddr;
	vmspace->vm_maxsaddr = vmspace_info.vm_maxsaddr;

	for (int i = 0; i < vmspace_info.nentries; i++) {

		/* Read an entry, add it to the map */
		error = fd_read(&entry_info, sizeof(struct vm_map_entry_info), fd);
		if (error)
			return error;

		/* 
		 * XXX If we need to create an entry that has no object, 
		 * we skip it for now, since these entries seem to have 
		 * size 0 anyway. There is relevant code from the kernel,
		 * but it's part of vm_object_allocate. We could just 
		 * allocate a vm_object of size 0 to back the entry. 
		 */
		if (entry_info.size == ULONG_MAX) {
			continue;
		}
		
		/* XXX Only works for anonymous objects */
		object = vm_object_allocate(OBJT_DEFAULT, entry_info.size);
		if (object == NULL) {
			printf("vm_object_allocate error\n");
			return (ENOMEM);
		}


		/* XXX Only we have a reference to the object, why take a lock? */
		VM_OBJECT_WLOCK(object);
		for (;;) {
	 		fd_read(&poffset, sizeof(vm_pindex_t), fd);
			/* Sentinel value, no more pages left to read */
			if (poffset == ULONG_MAX)
				break;

			page = vm_page_alloc(object, poffset, VM_ALLOC_NORMAL);
			if (page == NULL) {
				printf("vm_page_alloc error\n");
				return (ENOMEM);
			}

			vm_page_lock(page);
			vaddr = pmap_map(NULL, page->phys_addr, page->phys_addr + PAGE_SIZE,
                    VM_PROT_WRITE);

			error = fd_read((void *) vaddr, PAGE_SIZE, fd);
			pmap_enter(&vmspace->vm_pmap, 
                    entry_info.start + IDX_TO_OFF(poffset) - entry_info.offset,
                    page, entry_info.protection, VM_PROT_READ, 0);

			/* 
			 * Mark page as about to be used, keeping it from paging out. 
             * Probably doesn't do anything, but maybe... (It should be removed 
             * after we find the bug)
			 */
			vm_page_activate(page);
			vm_page_unlock(page);

			if (error) {
				printf("fd_read data error\n");
				VM_OBJECT_WUNLOCK(object);
				return error;
			}
		}
		VM_OBJECT_WUNLOCK(object);


		/* Enter the object into the map */
		vm_object_reference(object);
		vm_map_lock(map);
		/* XXX What flags should be used? */
		error = vm_map_insert(map, object, entry_info.offset, entry_info.start,
                entry_info.end, entry_info.protection, 
                entry_info.max_protection, MAP_COPY_ON_WRITE | MAP_PREFAULT);
	
		vm_map_unlock(map);

		if (error) {
			printf("vm_map_insert error\n");
			return error;
		}
	}
	return error;
}
