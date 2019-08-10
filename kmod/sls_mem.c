#include <sys/param.h>

#include <machine/param.h>

#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/namei.h>
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

#include "sls.h"
#include "slsmm.h"
#include "sls_path.h"
#include "sls_mem.h"

static int 
sls_vmobject_ckpt(struct proc *p, vm_object_t obj, struct sbuf *sb)
{
	struct vm_object_info cur_obj;
	int error;

	cur_obj.size = obj->size;
	cur_obj.type = obj->type;
	cur_obj.id = obj;
	cur_obj.backer = obj->backing_object;
	cur_obj.backer_off = obj->backing_object_offset;
	cur_obj.path = NULL;
	cur_obj.magic = SLS_OBJECT_INFO_MAGIC;

	error = sbuf_bcat(sb, (void *) &cur_obj, sizeof(cur_obj));
	if (error != 0)
	    return ENOMEM;

	/*
	* Used for mmap'd files - we are using the filename
	* to find out how to map.
	*/
	if (obj->type == OBJT_VNODE) {
	    PROC_UNLOCK(p);
	    error = sls_vn_to_path_append((struct vnode *) obj->handle, sb);
	    PROC_LOCK(p);
	    if (error == ENOENT) {
		printf("(BUG) Unlinked file found, ignoring for now\n");
		return 0;
	    }
	    if (error)
		return error;
	}


	return 0;
}

static int
sls_vmentry_ckpt(struct vm_map_entry *entry, struct sbuf *sb)
{
	struct vm_map_entry_info cur_entry;
	int error;

	cur_entry.magic = SLS_ENTRY_INFO_MAGIC;

	/* Grab vm_entry info. */
	cur_entry.start = entry->start;
	cur_entry.end = entry->end;
	cur_entry.offset = entry->offset;
	cur_entry.eflags = entry->eflags;
	cur_entry.protection = entry->protection;
	cur_entry.max_protection = entry->max_protection;
	cur_entry.inheritance = entry->inheritance;
	cur_entry.obj = entry->object.vm_object;
	if (cur_entry.obj != NULL)
	    cur_entry.type = cur_entry.obj->type;
	else
	    cur_entry.type = OBJT_DEAD;

	error = sbuf_bcat(sb, (void *) &cur_entry, sizeof(cur_entry));
	if (error != 0)
	    return ENOMEM;

	return 0;
}

int
sls_vmspace_ckpt(struct proc *p, struct sbuf *sb, long mode)
{
	vm_map_t vm_map;
	struct vmspace *vmspace;
	struct vm_map_entry *entry;
	struct vmspace_info vmspace_info;
	struct memckpt_info memory_info;
	struct vm_object_info obj_info;
	struct sbuf *objsb;
	size_t bufsize;
	vm_object_t *objects;
	vm_object_t obj;
	int error, i;

	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;

	vmspace_info = (struct vmspace_info) {
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

	memory_info = (struct memckpt_info) {
	    .magic = SLS_MEMCKPT_INFO_MAGIC,
	    .vmspace = vmspace_info,
	};

	error = sbuf_bcat(sb, (void *) &memory_info, sizeof(memory_info));
	if (error != 0)
	    return ENOMEM;


	for (entry = vm_map->header.next; entry != &vm_map->header; 
		entry = entry->next) {
	    /* 
	     * We checkpoint all ancestors of the top level objects.
	     * This way we may checkpoint the same object twice.
	     * We deduplicate at restore time.
	     *
	     * We need to restore backing objects before their shadows.
	     * For that reason, first traverse the chain then 
	     * checkpoint in the reverse order.
	     */
	    objsb = sbuf_new_auto();
	    for (obj = entry->object.vm_object; obj != NULL; 
		    obj = obj->backing_object) {

		error = sbuf_bcat(objsb, &obj, sizeof(obj));
		if (error != 0) {
		    sbuf_delete(objsb);
		    return error;
		
		}
	    }

	    error = sbuf_finish(objsb);
	    if (error != 0) {
		sbuf_delete(objsb);
		return error;
	    }
	    objects = (vm_object_t *) sbuf_data(objsb);
	    bufsize = sbuf_len(objsb) / sizeof(vm_object_t);

	    for (i = bufsize- 1; i >= 0; i--) {
		error = sls_vmobject_ckpt(p, objects[i], sb);
		if (error != 0) {
		    sbuf_delete(objsb);
		    return error;
		}
	    }

	    sbuf_delete(objsb);

	}

	/* Sentinel value, we are done with the objects. */
	obj_info.magic = SLS_OBJECTS_END;
	error = sbuf_bcat(sb, (void *) &obj_info, sizeof(obj_info));
	if (error != 0)
	    return ENOMEM;


	for (entry = vm_map->header.next; entry != &vm_map->header;
		entry = entry->next) {
	    error = sls_vmentry_ckpt(entry, sb);
	    if (error != 0)
		return error;
	}

	return 0;
}

/*
 * The same as vm_object_shadow, with different refcount handling and return values.
 * We also always create a shadow, regardless of the refcount.
 */
static vm_object_t 
sls_shadow(vm_object_t source, vm_ooffset_t offset, vm_size_t length)
{
	vm_object_t result;

	/*
	 * Store the offset into the source object, and fix up the offset into
	 * the new object.
	 */

	result = vm_object_allocate(OBJT_DEFAULT, atop(length));
	result->backing_object = source;
	result->backing_object_offset = offset;


	if (source != NULL) {
	    /* 
	    * If this is the first shadow, then we transfer the reference
	    * from the caller to the shadow, as done in vm_object_shadow.
	    * Otherwise we add a reference to the shadow.
	    */
	    if (source->shadow_count != 0)
		source->ref_count += 1;

		VM_OBJECT_WLOCK(source);
		result->domain = source->domain;
		LIST_INSERT_HEAD(&source->shadow_head, result, shadow_list);
		source->shadow_count++;
#if VM_NRESERVLEVEL > 0
		result->flags |= source->flags & OBJ_COLORED;
		result->pg_color = (source->pg_color + OFF_TO_IDX(offset)) &
		    ((1 << (VM_NFREEORDER - 1)) - 1);
#endif
		VM_OBJECT_WUNLOCK(source);
	}

	return result;
}

void
sls_data_rest(struct sls_pagetable ptable, struct vm_map *map, struct vm_map_entry *entry)
{

	struct dump_page *page_entry;
	vm_offset_t vaddr, addr;
	vm_pindex_t offset;
	vm_object_t object;
	vm_page_t new_page;
	int error, i;

	object = entry->object.vm_object;
	/* Non-anonymous objects can retrieve their pages from vnodes/devices */ 
	if (object == NULL || object->type != OBJT_DEFAULT)
	    return;

	for (i = 0; i <= ptable.hashmask; i++) {
	    LIST_FOREACH(page_entry, &ptable.pages[i & ptable.hashmask], next) {
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

		addr= pmap_map(NULL, new_page->phys_addr,
			new_page->phys_addr + PAGE_SIZE,
			VM_PROT_READ | VM_PROT_WRITE);
		memcpy((void *) addr, (void *) page_entry->data, PAGE_SIZE);
		/* pmap_delete or something? */

		error = pmap_enter(vm_map_pmap(map), vaddr, new_page,
			entry->protection, VM_PROT_READ, 0);
		if (error != 0) {
		    printf("Error: pmap_enter failed\n");
		}
		
		vm_page_xunbusy(new_page);

	    }
	}
}

int
sls_vmobject_rest(struct vm_object_info *info, struct sls_objtable *objtable)
{
	vm_object_t object, parent;
	struct vnode *vp;
	int error;

	/* Check if we have already restored it. */
	if (sls_objtable_find(info->id, objtable) != NULL)
	    return 0;

	switch (info->type) {
	case OBJT_DEFAULT:

	    /* Else grab the parent (if it exists) and shadow it. */
	    parent = sls_objtable_find(info->backer, objtable);
	    object = sls_shadow(parent, info->backer_off, ptoa(info->size));
	    sls_objtable_add(info->id, object, objtable);

	    return 0;

	case OBJT_VNODE:
	    /* 
	     * Just grab the object directly, so that we can find it 
	     * and shadow it. We deal with objects that are directly mapped 
	     * into the address space, as in the case of executables, 
	     * in vmentry_rest.
	     */

	    PROC_UNLOCK(curthread->td_proc);
	    error = sls_path_to_vn(info->path, &vp);
	    PROC_LOCK(curthread->td_proc);
	    if (error != 0) 
		return error;
	    
	    /* Get a reference for the vnode, since we're going to use it. */
	    vhold(vp);
	    object = vp->v_object;
	    sls_objtable_add(info->id, object, objtable);
	    return 0;

	/* 
	 * Device files either can't be mmap()'d, or have an mmap
	 * that maps anonymous objects, if they have the D_MMAP_ANON
	 * flag. In any case, we will never shadow an OBJT_DEVICE
	 * object, so we don't need it.
	 */
	case OBJT_DEVICE:
	    return 0;
	
	/* 
	 * Physical objects are unlikely to back other objects, but we
	 * play it safe. The only way it could happen would be if 
	 * a physical object had a VM_INHERIT_COPY inheritance flag.
	 */
	case OBJT_PHYS:

	    object = curthread->td_proc->p_sysent->sv_shared_page_obj;
	    if (object != NULL)
		sls_objtable_add(info->id, object, objtable);

	    return 0;

	default:
	    return EINVAL;
	}
}

/*
 * Variant of vm_map_insert, where we set the fields directly. 
 * Great for anonymous pages.
 */
static int
sls_vmentry_rest_anon(struct vm_map *map, struct vm_map_entry_info *entry, struct sls_objtable *objtable)
{
	vm_map_entry_t new_entry; 
	boolean_t contained;
	vm_object_t object;
	int error;

	object = sls_objtable_find(entry->obj, objtable);

	/* 
	 * Unfortunately, vm_map_entry_create is static, and so is
	 * the zone from which it allocates the entry. That means
	 * that we can only create entries using the API provided.
	 * The workaround until we are able to modify the kernel
	 * is to create a dummy map entry using vm_map_insert, and
	 * then manually set the fields we can't set by using the 
	 * proper arguments (mainly flags).
	 */
	vm_map_lock(map);
	error = vm_map_insert(map, object, entry->offset, 
		    entry->start, entry->end, 
		    entry->protection, entry->max_protection, 0);
	if (error != 0)
	    return error;

	/* Get the entry from the map. */
	contained = vm_map_lookup_entry(map, entry->start, &new_entry);
	if (contained == FALSE)
	    return EINVAL;

	new_entry->eflags = entry->eflags;
	new_entry->inheritance = entry->inheritance;

	/* XXX restore cred if needed */
	new_entry->cred = NULL;
	vm_map_unlock(map);


	/* 
	 * We could call use the prefault flag
	 * avoid minor faults after restoring.
	 */
	SLS_DBG("ANON\n");

	return 0;

}

static int
sls_vmentry_rest_file(struct vm_map *map, struct vm_map_entry_info *entry, struct sls_objtable *objtable)
{
	vm_object_t object; 
	struct vnode *vp;
	struct sbuf *sb = NULL;
	int fd = -1;
	int flags;
	int rprot;
	int error;

	/* If we have an object, it has to have been restored. */
	object = sls_objtable_find(entry->obj, objtable);
	if (object == NULL)
	    return EINVAL;

	vp = (struct vnode *) object->handle;
	error = sls_vn_to_path(vp, &sb);
	if (error != 0)
	    return EINVAL;

	if (entry->protection == VM_PROT_NONE) {
	    error = EPERM;
	    goto sls_vmentry_rest_file_done;
	}

	rprot = entry->protection & (VM_PROT_READ | VM_PROT_WRITE);
	if (rprot == VM_PROT_WRITE)
	    flags = O_WRONLY;
	else if (rprot == VM_PROT_READ)
	    flags = O_RDONLY;
	else
	    flags = O_RDWR;

	error = kern_openat(curthread, AT_FDCWD, sbuf_data(sb), 
		UIO_SYSSPACE, flags, S_IRWXU);
	if (error != 0)
	    goto sls_vmentry_rest_file_done;

	fd = curthread->td_retval[0];

	/* 
	 * These are the only flags that matter, the rest are about alignment. 
	 * MAP_SHARED is always on because otherwise we would have an 
	 * anonymous object mapping a vnode or device. We put MAP_FIXED 
	 * because we want the exact layout that we had previously.
	 */
	flags = MAP_SHARED | MAP_FIXED;
	if (entry->eflags & MAP_ENTRY_NOSYNC)
	    flags |= MAP_NOSYNC;
	if (entry->eflags & MAP_ENTRY_NOCOREDUMP)
	    flags |= MAP_NOCORE;

	error = kern_mmap(curthread, entry->start, entry->end - entry->start, 
		entry->protection, flags, fd, entry->offset);

sls_vmentry_rest_file_done:

	if (fd >= 0)
	    kern_close(curthread, fd);

	if (sb != NULL)
	    sbuf_delete(sb);

	return error;
}


int
sls_vmentry_rest(struct vm_map *map, struct vm_map_entry_info *entry, struct sls_objtable *objtable)
{
	int error;
	int fd;

	/* If it's a guard page use the code for anonymous/empty/physical entries. */
	if (entry->obj == NULL)
	    return sls_vmentry_rest_anon(map, entry, objtable);


	/* Jump table for restoring the entries. */
	switch (entry->type) {
	case OBJT_DEFAULT:
	    return sls_vmentry_rest_anon(map, entry, objtable);

	case OBJT_PHYS:
	    return sls_vmentry_rest_anon(map, entry, objtable);

	case OBJT_VNODE:
	    return sls_vmentry_rest_file(map, entry, objtable);

	/* 
	 * Right now we only support the HPET counter. If we 
	 * end up supporting more devices, we need to find 
	 * a way to get the names from the mapped objects.
	 * Is it even possible, though? Even in procstat 
	 * we don't see the name of the mapped device.
	 */
	case OBJT_DEVICE:
	    error = kern_openat(curthread, AT_FDCWD, "/dev/hpet0",
		    UIO_SYSSPACE, O_RDWR, S_IRWXU);
	    if (error != 0)
		return error;

	    fd = curthread->td_retval[0];
	    error = kern_mmap(curthread, entry->start, entry->end - entry->start, 
		    entry->protection, MAP_SHARED, fd, entry->offset);

	    kern_close(curthread, fd);
	    return error;

	default:
	    return EINVAL;
	}
}

int
sls_vmspace_rest(struct proc *p, struct memckpt_info memckpt)
{
	struct vmspace *vmspace;
	struct vm_map *vm_map;

	/* Shorthands */
	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;

	/*
	 * Blow away the old address space, as done in exec_new_vmspace
	 */
	/* XXX We have to look further into how to handle System V shmem */
	/* XXX Only FreeBSD binaries for now*/
	shmexit(vmspace);
	pmap_remove_pages(vmspace_pmap(vmspace));
	vm_map_remove(vm_map, vm_map_min(vm_map), vm_map_max(vm_map));
	vm_map_lock(vm_map);
	vm_map_modflags(vm_map, 0, MAP_WIREFUTURE);
	vm_map_unlock(vm_map);

	/* Refresh the value of vmspace in case it changed above */
	vmspace = p->p_vmspace;

	/* Copy vmspace state to the existing vmspace */
	vmspace->vm_swrss = memckpt.vmspace.vm_swrss;
	vmspace->vm_tsize = memckpt.vmspace.vm_tsize;
	vmspace->vm_dsize = memckpt.vmspace.vm_dsize;
	vmspace->vm_ssize = memckpt.vmspace.vm_ssize;
	vmspace->vm_taddr = memckpt.vmspace.vm_taddr;
	vmspace->vm_taddr = memckpt.vmspace.vm_daddr;
	vmspace->vm_maxsaddr = memckpt.vmspace.vm_maxsaddr; 

	return 0;
}
