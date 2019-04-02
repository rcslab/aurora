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
#include "path.h"
#include "sls_mem.h"
#include "sls_snapshot.h"
#include "sls_dump.h"

/* Hack for supporting the hpet0 counter */
static struct cdev *sls_hpet0 = NULL;

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

/*
 * XXX In the future, get _all_ objects, do not compact. Right now, 
 * we basically flatten the object tree. This makes sense in that 
 * we only checkpoint one process, so the only logical relation
 * between objects is that of an anonymous object backed by 
 * a vnode. 
 *
 * Of course this assumes that a) there are no _illogical_ relations,
 * i.e. having a non-leaf object directly exposed. It also causes 
 * performance penalties if there are weird complex mappings in such
 * a way that two objects have a data-heavy common ancestor. Right
 * now, its pages would be copied twice.
 */
static int 
vm_object_ckpt(struct proc *p, vm_object_t obj, struct vm_object_info **obj_info)
{
	vm_object_t o;
	struct vm_object_info *cur_obj;
	char *filepath;
	size_t filepath_len;
	struct vnode *vp;
	int error;

	cur_obj = malloc(sizeof(*cur_obj), M_SLSMM, M_NOWAIT);
	if (cur_obj == NULL)
	    return ENOMEM;

	cur_obj->size = obj->size;
	cur_obj->type = obj->type;
	cur_obj->id = (vm_offset_t) obj;
	cur_obj->backer = 0;
	cur_obj->backer_off = obj->backing_object_offset;
	cur_obj->filename_len = 0;
	cur_obj->filename = NULL;
	cur_obj->magic = SLS_OBJECT_INFO_MAGIC;

	/* Get the total backing offset for the deepest non-anonymous object */
	for (o = obj->backing_object; o != NULL; o = o->backing_object) {
	    if (o->type != OBJT_DEFAULT)
		break;

	    cur_obj->backer_off += o->backing_object_offset;
	}

	/* Only care about non-anonymous objects */
	if (o != NULL) {
	    cur_obj->backer = (vm_offset_t) o;
	} else
	    cur_obj->backer_off = 0;

	/*
	* Used for mmap'd files - we are using the filename
	* to find out how to map.
	*/
	if (obj->type == OBJT_VNODE) {
	    vp = (struct vnode *) obj->handle;

	    PROC_UNLOCK(p);
	    error = vnode_to_filename(vp, &filepath, &filepath_len);
	    PROC_LOCK(p);
	    if (error) {
		printf("vnode_to_filename failed with code %d\n", error);
		free(cur_obj, M_SLSMM);
		return error;
	    }

	    cur_obj->filename_len = filepath_len;
	    cur_obj->filename = filepath;

	} else if (obj->type == OBJT_DEVICE) {
	    /* 
	     * XXX There seems to be _no_way_  to get the name from the device. 
	     * Without the interposition layer, this hack seems to be the 
	     * only way to go. 
	     */
	    sls_hpet0 = (struct cdev *) obj->handle;
	} 

	*obj_info = cur_obj;

	return 0;
}

/* insert a shadow object to each vm_object in a vmspace,
 * return the list of original vm_object and vm_entry for dump
 */
int
vmspace_ckpt(struct proc *p, struct memckpt_info *dump, long mode)
{
	vm_map_t vm_map;
	struct vmspace *vmspace;
	struct vm_map_entry *entry;
	struct vm_map_entry_info *entries;
	struct vm_map_entry_info *cur_entry;
	vm_object_t obj;
	int i;

	vmspace = p->p_vmspace;

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


	    /* Grab vm_entry info. */
	    cur_entry->start = entry->start;
	    cur_entry->end = entry->end;
	    cur_entry->offset = entry->offset;
	    cur_entry->eflags = entry->eflags;
	    cur_entry->protection = entry->protection;
	    cur_entry->max_protection = entry->max_protection;
	    cur_entry->obj_info = NULL;

	    obj = entry->object.vm_object;
	    if (obj == NULL)
		continue;

	    vm_object_ckpt(p, obj, &cur_entry->obj_info);

	}

	return 0;
}


static vm_object_t
vm_object_rest(struct proc *p, struct vm_map_entry_info *entry, vm_object_t backer,
		    int *flags, boolean_t *writecounted)
{
	struct vm_object_info *obj_info;
	vm_object_t new_object = NULL;
	vm_offset_t offset;
	struct cdevsw *dsw;
	struct vnode *vp;
	int ref;
	int error;
	struct thread *td;

	/* Needed for restoring physical pages */
	td = TAILQ_FIRST(&p->p_threads);

	obj_info = entry->obj_info;
	switch (obj_info->type) {
	    case OBJT_DEFAULT:

		/* This object is shadowing another object we just created */
		if (backer != NULL) {
		    offset = obj_info->backer_off;
		    /* Watch out, backer is local to this function */
		    vm_object_reference(backer);
		    vm_object_shadow(&backer, &offset, obj_info->size);
		    return backer;
		}

		return vm_object_allocate(OBJT_DEFAULT, obj_info->size);

	    case OBJT_VNODE:

		PROC_UNLOCK(curthread->td_proc);
		error = filename_to_vnode(obj_info->filename, &vp);
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
		/* Map a shared page */
		new_object = p->p_sysent->sv_shared_page_obj;
		if (new_object != NULL) {
			vm_object_reference(new_object);
		}


		return new_object;

	    case OBJT_DEVICE:
		/* 
		* XXX This is a precarious operation, since few devices can
		* be unmapped and remapped on a whim. Still, /dev/hpet0 should
		* work, since it does not really interact with processes, but 
		* exposes CPU-specific counters. Since hpet0 is used by glibc,
		* it's important that we support it.
 		*/
		dsw = dev_refthread(sls_hpet0, &ref);
		if (dsw == NULL) {
		    panic("Device restoration failed\n");
		}

		error = vm_mmap_cdev(td, entry->end - entry->start,
			entry->protection, &entry->max_protection,
			flags, sls_hpet0, dsw, &entry->offset,
			&new_object);
		if (error) {
		    printf("Error: vm_mmap_vnode failed with error %d\n", error);
		}

		dev_relthread(sls_hpet0, ref);

		return new_object;

	    case OBJT_DEAD:
		printf("Warning: Dead object found\n");
		/* Guard obj_info */
		if (obj_info->size == ULONG_MAX)
		    return NULL;

		/* FALLTHROUGH */
	    default:
		printf("Error: Invalid vm_object type %d\n", obj_info->type);
		return new_object;
	}
}

static void
data_rest(struct sls_snapshot *slss, struct vm_map *map, vm_object_t object, struct vm_map_entry_info *entry)
{

	struct dump_page *page_entry;
	vm_offset_t vaddr, addr __unused;
	u_long hashmask;
	vm_pindex_t offset;
	vm_page_t new_page;
	int error;

	hashmask = slss->slss_hashmask;

	for (int j = 0; j <= hashmask; j++) {
	    LIST_FOREACH(page_entry, &slss->slss_pages[j & hashmask], next) {
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

static int
find_obj_id(struct vm_map_entry_info *entries, int numentries, vm_offset_t id)
{
    int i;
    struct vm_object_info *obj_info;

    for (i = 0; i < numentries; i++) {
	obj_info = entries[i].obj_info;
	if (obj_info == NULL) 
	    continue;
	    
	if (obj_info->id == id)
	    return i;
    }

    return -1;
}

int
vmspace_rest(struct proc *p, struct sls_snapshot *slss, struct memckpt_info *dump)
{
	struct vmspace *vmspace;
	struct vm_map *vm_map;
	struct vm_map_entry_info *entry;
	struct vm_object_info *obj_info;
	vm_object_t new_object;
	//vm_offset_t addr;
	int error = 0;
	int cow;
	boolean_t writecounted;
	int flags;
	int numentries;
	vm_object_t *objects;
	vm_object_t backer;
	int i, index;


	/* Shorthands */
	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;
	numentries = dump->vmspace.nentries;

	/* The newly created objects */
	objects = malloc(sizeof(*objects) * numentries, M_SLSMM, M_NOWAIT);
	if (objects == NULL)
	    return ENOMEM;

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
	for (i = 0; i < numentries; i++) {

	    entry = &dump->entries[i];
	    obj_info = entry->obj_info;

	    if (obj_info == NULL) {
		cow = map_flags_from_entry(entry->eflags);
		PROC_UNLOCK(p);
		vm_map_lock(vm_map);
		error = vm_map_insert(vm_map, NULL, 0, 
					entry->start, entry->end,
					entry->protection, entry->max_protection,
					cow);
		vm_map_unlock(vm_map);
		PROC_LOCK(p);
		if (error != KERN_SUCCESS) {
		    printf("Error: vm_map_insert failed with %d\n", 
						    vm_mmap_to_errno(error));
		    free(objects, M_SLSMM);
		    return error;
		}

		continue;
	    }

	    /* XXX HACK  */
	    /*
	    if (obj_info->type == OBJT_DEVICE) {
		printf("12.0 objt device vm_object hack. Ignoring object.\n");
		continue;
	    }
	    */

	    /* 
	     * XXX We assume that the backer has already been restored. 
	     * We can relax that by postponing the restoration of this object
	     * until the backer is in place.
	     */
	    backer = NULL;
	    if (obj_info->backer != 0) {
		index = find_obj_id(dump->entries, numentries, obj_info->backer);
		if (index != -1)
		    backer = objects[index];
		else
		    printf("ERROR: Backer object for entry %d not found", i);

		if (index > i)
		    printf("ERROR: Backer not restored yet, crash imminent\n");
	    }


	    /*
	     * We can have a new_object that is null, in fact this is how
	     * we would normally create an anonymous mapping.
	     */
	    writecounted = FALSE;
	    flags = 0;
	    new_object = vm_object_rest(p, entry, backer, &flags, &writecounted);
	    objects[i] = new_object;

	    cow = map_flags_from_entry(entry->eflags);
	    PROC_UNLOCK(p);
	    vm_map_lock(vm_map);
	    error = vm_map_insert(vm_map, new_object, entry->offset,
		    entry->start, entry->end,
		    entry->protection, entry->max_protection,
		    cow);
	    vm_map_unlock(vm_map);
	    PROC_LOCK(p);
	    if (error != KERN_SUCCESS) {
		printf("Error: vm_map_insert failed with %d\n", vm_mmap_to_errno(error));
		free(objects, M_SLSMM);
		return vm_mmap_to_errno(error);
	    }

	    
	    if (new_object && new_object->type == OBJT_DEFAULT)
		data_rest(slss, vm_map, new_object, entry);
	}
	    

	free(objects, M_SLSMM);

	return 0;
}
