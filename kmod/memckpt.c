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
 * XXX Small function or define for these two? I went with 
 * define because the operation is simple enough, and
 * so that we are free to use an uppercase name that 
 * denotes a simple calculation.
 */
#define IDX_TO_VADDR(idx, entry_start, entry_offset) \
	(IDX_TO_OFF(idx) + entry_start - entry_offset)
#define VADDR_TO_IDX(vaddr, entry_start, entry_offset) \
	(OFF_TO_IDX(vaddr - entry_start + entry_offset))

/* Map a user memory area to kernelspace. */
vm_offset_t
userpage_map(vm_paddr_t phys_addr)
{
	return pmap_map(NULL, phys_addr, phys_addr + PAGE_SIZE, 
				VM_PROT_READ | VM_PROT_WRITE);
}


/* Currently a no-op, will be needed if we ever port to other archs */
void
userpage_unmap(vm_offset_t vaddr)
{
}


/* insert a shadow object to each vm_object in a vmspace,
 * return the list of original vm_object and vm_entry for dump
 */
int
vmspace_checkpoint(struct vmspace *vmspace, struct dump *dump, long mode)
{
	vm_map_t vm_map; 
	struct vm_map_entry *entry;
	vm_object_t *objects; 
	struct vm_map_entry_info *entries; 
	vm_pindex_t object_size;
	int error = 0;
	int i; 


	/* Shorthands */
	objects = dump->objects;
	entries = dump->entries;
	vm_map = &vmspace->vm_map;

	dump->vmspace = (struct vmspace_info) {
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

	for (entry = vm_map->header.next, i = 0; entry != &vm_map->header; 
	     entry = entry->next, i++) {

		/* 
		 * Grab vm_object. If the entry is unbacked, write a sentinel
		 * value in the object_size field.
		 */
		objects[i] = entry->object.vm_object;
		if (objects[i]) 
			object_size = objects[i]->size;
		else 
			object_size = ULONG_MAX;

		/* Grab vm_entry info. */
		entries[i].start = entry->start;
		entries[i].end = entry->end;
		entries[i].offset = entry->offset;
		entries[i].eflags = entry->eflags;
		entries[i].protection = entry->protection;
		entries[i].max_protection = entry->max_protection;
		entries[i].size = object_size;
		entries[i].magic = SLS_ENTRY_INFO_MAGIC;

		/*
		 * If in delta mode, we are playing tricks with object chains.
		 * After we get an object, we should be doing the following
		 * to the address space:
		 *
		 * Let A the original object (fully checkpointed). We create 
		 * shadow B at checkpoint time, which at delta checkpoint time 
		 * will hold all pages modified since the last checkpoint. We should 
		 * save only B's pages, then flush them back to A. Normally this is 
		 * done by creating a shadow of B, then dumping the data of B and
		 * freeing it. By freeing it, the chain collapses 
		 *
		 * We also remove all mappings from the pmap, so that the new shadow
		 * has to read the data from its backing object.
		 *
		 * XXX Actually verify that this is what the code below does, would
		 * be great to have a way to print the chain at any given time,
		 * since we are soon going into unmapped territory with this
		 * (i.e. fork).
		 */
		if (mode == DELTA_DUMP) {
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
vmspace_dump(struct dump *dump, int fd, long mode) 
{
	vm_page_t page;
	vm_pindex_t poffset;
	vm_offset_t vaddr, vaddr_data;
	vm_object_t *objects; 
	struct vm_map_entry_info *entries; 
	int error = 0;
	int i;

	/* Shorthands */
	objects = dump->objects;
	entries = dump->entries;

	error = fd_write(&dump->vmspace, sizeof(struct vmspace_info), fd);
	if (error)
		return error;

	for (i = 0; i < dump->vmspace.nentries; i++) {

		if (entries->eflags & MAP_ENTRY_IS_SUB_MAP) {
			printf("WARNING: Submap entry found, dump will be wrong\n");
			continue;
		}

		error = fd_write(&entries[i], sizeof(struct vm_map_entry_info), fd);
		if (error)
			return error;


		if (objects[i] == NULL) 
			continue;

		TAILQ_FOREACH(page, &objects[i]->memq, listq) {
			/*
			 * XXX Does this check make sense? We _are_ getting pages
			 * from a valid object, after all, why would it have NULL
			 * pointers in its list?
			 */
			if (!page) {
				printf("ERROR: vm_page_t page is NULL");
				continue;
			}

			/* 
			 * Map the page to the address space, and save a virtual address - 
			 * data pair to disk.
			 */
			vaddr_data = IDX_TO_VADDR(page->pindex, entries[i].start, entries[i].offset);
			error = fd_write(&vaddr_data, sizeof(vm_offset_t), fd);
			if (error)
				return error;

			vaddr = userpage_map(page->phys_addr);
			if (!vaddr)
				return error;

			error = fd_write((void*) vaddr, PAGE_SIZE, fd);
			if (error)
				return error;

			userpage_unmap(vaddr);

        	}

		/* Sentinel value that denotes there are no more pages */
		poffset = ULONG_MAX;
		error = fd_write(&poffset, sizeof(vm_offset_t), fd);
		if (error)
			return error;

		/* For delta dumps, deallocate the object to collapse the chain again. */
		if (mode == DELTA_DUMP) 
			vm_object_deallocate(objects[i]);
	}

	return error;
}

int
vmspace_restore(struct proc *p, struct dump *dump)
{
	struct vmspace *vmspace; 
	struct vm_map *vm_map;
	struct vm_map_entry_info *cur_entry;
	vm_object_t new_object;
	vm_pindex_t offset;
	vm_page_t new_page; 
	vm_offset_t vaddr; 
	int error = 0;

	/* Shorthands */
	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;

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
	vm_map_remove(vm_map, vm_map_min(vm_map), vm_map_max(vm_map));

	/* XXX We have to look further into how to handle System V shmem */
	shmexit(vmspace);

	/* If all pages were wired using mlockall(), undo that */
	vm_map_lock(vm_map);
	vm_map_modflags(vm_map, 0, MAP_WIREFUTURE); 
	vm_map_unlock(vm_map);


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
		struct dump_page *page_entry;

		/*
		 * We assume no entry will span the whole virtual address space,
		 * so ULONG_MAX is a sentinel value for when there is no object
		 * backing an entry. We do not know why there are entries without 
		 * a backing object, to be honest.
		 */

		cur_entry = &dump->entries[i];
		if (cur_entry->size == ULONG_MAX)
			continue;

		/* XXX Repair file mappings */
		new_object = vm_object_allocate(OBJT_DEFAULT, cur_entry->size);
		if (!new_object) {
			printf("Error: vm_object_allocate failed\n");
			return ENOMEM;
		}


		/* 
		 * Traverse _everything_. This can be obviously be 
		 * improved upon by using the higher (middle,
		 * since the higher are basically unused) instead of the lower bits 
		 * of an address when hashing, and also using more buckets.
		 */

		VM_OBJECT_WLOCK(new_object);
		for (int j = 0; j <= hashmask; j++) {

			LIST_FOREACH(page_entry, &slspages[j & hashmask], next) {
				if (cur_entry->start <=  page_entry->vaddr && 
					 	page_entry->vaddr < cur_entry->end) {

					/* 
					 * We cannot add the page we have saved the data in
					 * to the object, because it currently belongs to 
					 * the kernel. So we get another one, copy the contents
					 * to it, and add the mapping to the page tables.
					 */

					offset = VADDR_TO_IDX(page_entry->vaddr, 
							cur_entry->start, cur_entry->offset);
					new_page = vm_page_grab(new_object, offset, VM_ALLOC_NORMAL);

					vaddr = userpage_map(new_page->phys_addr);
					memcpy((void *) vaddr, (void *) page_entry->data, PAGE_SIZE);
					userpage_unmap(vaddr);

					pmap_enter(vmspace_pmap(vmspace), page_entry->vaddr, new_page, 
							cur_entry->protection, VM_PROT_READ, 0);
						
				}
			}

		}
		VM_OBJECT_WUNLOCK(new_object);

		/* The actual place where we add the entry to userspace */

		vm_map_lock(vm_map);
		error = vm_map_insert(vm_map, new_object, cur_entry->offset, 
				cur_entry->start, cur_entry->end, 
				cur_entry->protection, cur_entry->max_protection, 
				MAP_COPY_ON_WRITE | MAP_PREFAULT);
		vm_map_unlock(vm_map);

		if (error) {
			printf("vm_map_insert error\n");
			return error;
		}
	}
	
	return error;
}
