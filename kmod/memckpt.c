#include <sys/param.h>

#include <machine/param.h>

#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
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


#include "memckpt.h"
#include "_slsmm.h"
#include "slsmm.h"
#include "path.h"
#include "backends/fileio.h"


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
vmspace_checkpoint(struct vmspace *vmspace, struct memckpt_info *dump, vm_object_t *objects, long mode)
{
	vm_map_t vm_map;
	struct vm_map_entry *entry;
	struct vm_map_entry_info *entries;
	struct vm_map_entry_info *cur_entry;
	char *filepath;
	size_t filepath_len;
	struct vnode *vp;
	vm_object_t obj;
	int error = 0;
	int i;


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

	    cur_entry = &entries[i];
	    cur_entry->magic = SLS_ENTRY_INFO_MAGIC;

	    /*
	    * Grab vm_object. Write sentinel values in the fields
	    * related to the object associated with the entry, to be
	    * overwritten with valid data if such an object exists;
	    */

	    /* Grab vm_entry info. */
	    cur_entry->start = entry->start;
	    cur_entry->end = entry->end;
	    cur_entry->offset = entry->offset;
	    cur_entry->eflags = entry->eflags;
	    cur_entry->protection = entry->protection;
	    cur_entry->max_protection = entry->max_protection;

	    /*
	    * Sentinel values, will be overwritten for entries
	    * that are backed by objects.
	    */
	    cur_entry->size = ULONG_MAX;
	    cur_entry->resident_page_count = UINT_MAX;
	    cur_entry->file_offset = ULONG_MAX;
	    cur_entry->type = OBJT_DEAD;

	    cur_entry->file_offset = 0;
	    cur_entry->filename_len = 0;
	    cur_entry->filename = NULL;

	    cur_entry->is_shadow = false;
	    cur_entry->backing_offset = 0;
	    cur_entry->backing_object =  NULL;
	    cur_entry->current_object =  NULL;


	    obj = objects[i] = entry->object.vm_object;
	    if (obj) {
		/* XXX Shadow hack */
		cur_entry->current_object = obj;

		cur_entry->size = obj->size;
		cur_entry->resident_page_count = obj->resident_page_count;
		cur_entry->type= obj->type;

		/*
		* Used for mmap'd files - we are using the filename
		* to find out how to map.
		*/
		if (cur_entry->type == OBJT_VNODE) {
		    vp = (struct vnode *) obj->handle;

		    error = vnode_to_filename(vp, &filepath, &filepath_len);
		    if (error) {
			printf("vnode_to_filename failed with code %d\n", error);
			return error;

		    }

		    /* FIXME Probably wrong, offset is probably in the object */
		    cur_entry->file_offset = 0;


		    cur_entry->filename_len = filepath_len;
		    cur_entry->filename = filepath;
		} else if (obj->backing_object != NULL) {
		    /* Always OBJT_DEFAULT (look at vm_object_shadow())*/

		    cur_entry->is_shadow = true;
		    cur_entry->backing_offset =
			obj->backing_object_offset;
		    cur_entry->backing_object = obj->backing_object;

		}


	    }


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
	    if (mode == SLSMM_CKPT_DELTA && objects[i]) {
		vm_object_reference(entry->object.vm_object);
		vm_object_shadow(&entry->object.vm_object, &entry->offset,
			entry->end-entry->start);
		pmap_remove(vm_map->pmap, entry->start, entry->end);
	    }
	}

	return 0;
}

/*
 * XXX Shadowing hack for vmspace_restore, defined here in order not to not
 * put pressure on the stack. If this works, replace with a hashtable.
 */
#define HACK_MAX_ENTRIES 128
/* Linear search to find the proper index */
vm_object_t entry_id[HACK_MAX_ENTRIES];
/* Use the index here ot get the object to be shadowed */
vm_object_t entry_obj[HACK_MAX_ENTRIES];
/* (Assume shadows always get retrieved after the original object) */




static vm_object_t
vm_object_restore(struct vm_map_entry_info *entry, int *flags, boolean_t *writecounted)
{
	vm_object_t new_object = NULL;
	struct vnode *vp;
	int error;

	switch (entry->type) {
	    case OBJT_DEFAULT:

		/* XXX Shadow hack */
		if (entry->is_shadow) {
		    vm_object_t old_object = NULL;
		    for (int i = 0; i < HACK_MAX_ENTRIES; i++) {
			if (entry->backing_object == entry_id[i]) {
			    old_object = entry_obj[i];
			    break;
			}
		    }


		    vm_offset_t offset = entry->backing_offset;
		    int len = entry->end - entry->start;
		    vm_object_shadow(&old_object, &offset, len);

		    return old_object;
		}

		return vm_object_allocate(OBJT_DEFAULT, entry->size);

	    case OBJT_VNODE:

		PROC_UNLOCK(curthread->td_proc);
		error = filename_to_vnode(entry->filename, &vp);
		PROC_LOCK(curthread->td_proc);
		if (error) {
		    printf("Error: filename_to_vnode failed with %d\n", error);
		    return NULL;
		}


		/*
		* XXX vm_mmap_vnode() has a ton of useful checks, but
		* it seems like just grabbing the object also works.
		*/
		error = vm_mmap_vnode(curthread, entry->end - entry->start,
			entry->protection, &entry->max_protection,
			flags, vp, &entry->offset,
			&new_object, writecounted);
		if (error) {
		    printf("Error: vm_mmap_vnode failed with error %d\n", error);
		}

		vm_object_reference(new_object);

		return new_object;
	    case OBJT_PHYS:
		/*
		* XXX Actually mimic the linker and use physical pages
		*/
		return vm_object_allocate(OBJT_DEFAULT, entry->size);

	    case OBJT_DEAD:
		/* Guard entry */
		if (entry->size == ULONG_MAX)
		    return NULL;

		/* FALLTHROUGH */
	    default:
		printf("Error: Invalid vm_object type %d\n", entry->type);
		return new_object;
	}
}

static void
data_restore(struct vm_map *map, vm_object_t object, struct vm_map_entry_info *entry)
{

	struct dump_page *page_entry;
	vm_offset_t vaddr, addr __unused;
	vm_pindex_t offset;
	vm_page_t new_page;
	int error;

	/*
	* Traverse _everything_. This can be obviously be
	* improved upon by using the higher bits
	* of an address when hashing, and also using more buckets.
	*/

	for (int j = 0; j <= hashmask; j++) {
	    LIST_FOREACH(page_entry, &slspages[j & hashmask], next) {
		vaddr = page_entry->vaddr;
		if (vaddr < entry->start || vaddr >= entry->end)
		    continue;

		/*
		* We cannot add the page we have saved the data in
		* to the object, because it currently belongs to
		* the kernel. So we get another one, copy the contents
		* to it, and add the mapping to the page tables.
		*/

		offset = VADDR_TO_IDX(vaddr, entry->start, entry->offset);

		VM_OBJECT_WLOCK(object);
		new_page = vm_page_alloc(object, offset, VM_ALLOC_NORMAL);

		if (new_page == NULL) {
		    printf("Error: vm_page_grab failed\n");
		    continue;
		}

		/* XXX Checkpoint and restore page valid/dirty bits? */
		/*
		if (new_page->valid != VM_PAGE_BITS_ALL) {
		    error = vm_pager_get_pages(object, &new_page, 1, NULL, NULL);
		    if (error != VM_PAGER_OK)
			panic("page could not be retrieved\n");

		}
		*/

		new_page->valid = VM_PAGE_BITS_ALL;

		VM_OBJECT_WUNLOCK(object);

		addr = userpage_map(new_page->phys_addr);
		memcpy((void *) addr, (void *) page_entry->data, PAGE_SIZE);
		userpage_unmap(addr);

		error = pmap_enter(vm_map_pmap(map), vaddr, new_page,
			entry->protection, VM_PROT_READ, 0);
		if (error != 0) {
		    printf("Error: pmap_enter failed\n");
		}
		
		vm_page_xunbusy(new_page);


	    }

	}
}

/*
 * Work backwards from the entry flags to find out what
 * flags exactly we need to pass to vm_map_insert. Normally
 * we'd just create the entry directly, but the methods we
 * need are static. TODO: Find out what exactly we need
 */
static int
map_flags_from_entry(vm_eflags_t flags)
{
	int cow = 0;

	if (flags & (MAP_ENTRY_COW | MAP_ENTRY_NEEDS_COPY))
	    cow |= MAP_COPY_ON_WRITE;
	if (flags & MAP_ENTRY_NOFAULT)
	    cow |= MAP_NOFAULT;
	if (flags & MAP_ENTRY_NOSYNC)
	    cow |= MAP_DISABLE_SYNCER;
	if (flags & MAP_ENTRY_NOCOREDUMP)
	    cow |= MAP_DISABLE_COREDUMP;
	if (flags & MAP_ENTRY_GROWS_DOWN)
	    cow |= MAP_STACK_GROWS_DOWN;
	if (flags & MAP_ENTRY_GROWS_UP)
	    cow |= MAP_STACK_GROWS_UP;
	if (flags & MAP_ENTRY_VN_WRITECNT)
	    cow |= MAP_VN_WRITECOUNT;
	if ((flags & MAP_ENTRY_GUARD) != 0)
	    cow |= MAP_CREATE_GUARD;
	if ((flags & MAP_ENTRY_STACK_GAP_DN) != 0)
	    cow |= MAP_CREATE_STACK_GAP_DN;
	if ((flags & MAP_ENTRY_STACK_GAP_UP) != 0)
	    cow |= MAP_CREATE_STACK_GAP_UP;


	return cow;
}

int
vmspace_restore(struct proc *p, struct memckpt_info *dump)
{
	struct vmspace *vmspace;
	struct vm_map *vm_map;
	struct vm_map_entry_info *entry;
	vm_object_t new_object;
	//vm_offset_t addr;
	int error = 0;
	int cow;
	boolean_t writecounted;
	int flags;


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
	/* XXX We have to look further into how to handle System V shmem */
	/* XXX Only FreeBSD binaries for now*/
	/* XXX vmspace_exec does _not_ work rn */
	shmexit(vmspace);
	pmap_remove_pages(vmspace_pmap(vmspace));
	vm_map_remove(vm_map, vm_map_min(vm_map), vm_map_max(vm_map));
	vm_map_lock(vm_map);
	vm_map_modflags(vm_map, 0, MAP_WIREFUTURE);
	vm_map_unlock(vm_map);

	/* Refresh the values in case they changed above */
	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;


	/* Copy vmspace state to the existing vmspace */
	vmspace->vm_swrss = dump->vmspace.vm_swrss;
	vmspace->vm_tsize = dump->vmspace.vm_tsize;
	vmspace->vm_dsize = dump->vmspace.vm_dsize;
	vmspace->vm_ssize = dump->vmspace.vm_ssize;
	vmspace->vm_taddr = dump->vmspace.vm_taddr;
	vmspace->vm_taddr = dump->vmspace.vm_daddr;
	vmspace->vm_maxsaddr = dump->vmspace.vm_maxsaddr;


	/* restore vm_map entries */
	for (int i = 0; i < dump->vmspace.nentries; i++) {

	    printf("object being restored:entry %d\n", i);
	    /*
	     * We assume no entry will span the whole virtual address space,
	     * so ULONG_MAX is a sentinel value for when there is no object
	     * backing an entry. We do not know why there are entries without
	     * a backing object, to be honest.
	     */

	    entry = &dump->entries[i];

	    /* XXX Shadow hack */
	    entry_id[i] = entry->current_object;

	    writecounted = FALSE;
	    flags = 0;

	    /*XXX HACK  */
	    if (entry->type == OBJT_DEVICE) {
		printf("12.0 objt device vm_object hack. Ignoring object.\n");
		continue;
	    }

	    /*
	     * We can have a new_object that is null, in fact this is how
	     * we would normally create an anonymous mapping.
	     */
	    new_object = vm_object_restore(entry, &flags, &writecounted);

	    /* XXX Shadow hack */
	    entry_obj[i] = new_object;


	    cow = map_flags_from_entry(entry->eflags);
	    vm_map_lock(vm_map);
	    error = vm_map_insert(vm_map, new_object, entry->offset,
		    entry->start, entry->end,
		    entry->protection, entry->max_protection,
		    cow);
	    vm_map_unlock(vm_map);
	    if (error) {
		printf("Error: vm_map_insert failed with %d\n", error);
		return error;
	    }

	    
	    if (new_object && new_object->type != OBJT_VNODE)
		data_restore(vm_map, new_object, entry);
	    printf("restored object: %d\n", i);
	}
	    


	return 0;
}
