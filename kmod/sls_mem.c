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
#include "slskv.h"
#include "slsmm.h"
#include "slstable.h"
#include "sls_path.h"
#include "sls_mem.h"

#include <slos.h>

static int 
sls_vmobject_ckpt(struct proc *p, vm_object_t obj)
{
	struct vm_object_info cur_obj;
	struct sbuf *sb;
	int error;

	/* Find if we have already checkpointed the object. */
	if (slskv_find(slsm.slsm_rectable, (uint64_t) obj, (uintptr_t *) &sb) == 0)
	    return 0;

	/* First time we come across it, create a buffer for the info struct. */
	sb = sbuf_new_auto();

	cur_obj.size = obj->size;
	cur_obj.type = obj->type;
	cur_obj.id = obj;
	cur_obj.backer = obj->backing_object;
	cur_obj.backer_off = obj->backing_object_offset;
	cur_obj.path = NULL;
	cur_obj.magic = SLS_OBJECT_INFO_MAGIC;
	cur_obj.slsid = (uint64_t) obj;

	error = sbuf_bcat(sb, (void *) &cur_obj, sizeof(cur_obj));
	if (error != 0)
	    goto error;

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

	    if (error != 0)
		goto error;
	}


	error = sbuf_finish(sb);
	if (error != 0)
	    return error;

	error = slskv_add(slsm.slsm_rectable, (uint64_t) obj, (uintptr_t) sb);
	if (error != 0)
	    goto error;

	error = slskv_add(slsm.slsm_typetable, (uint64_t) sb, (uintptr_t) SLOSREC_VMOBJ);
	if (error != 0)
	    goto error;

	return 0;

error:

	slskv_del(slsm.slsm_rectable, (uint64_t) obj);
	sbuf_delete(sb);

	return error;
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
	cur_entry.slsid = (uint64_t) entry;
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
	struct slsvmspace slsvmspace;
	vm_object_t obj;
	int error, i;

	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;

	slsvmspace = (struct vmspace_info) {
	    .magic = SLS_VMSPACE_INFO_MAGIC,
	    .slsid = (uint64_t) vmspace,
	    .vm_swrss = vmspace->vm_swrss,
	    .vm_tsize = vmspace->vm_tsize,
	    .vm_dsize = vmspace->vm_dsize,
	    .vm_ssize = vmspace->vm_ssize,
	    .vm_taddr = vmspace->vm_taddr,
	    .vm_daddr = vmspace->vm_daddr,
	    .vm_maxsaddr = vmspace->vm_maxsaddr,
	    .nentries = vm_map->nentries,
	};

	error = sbuf_bcat(sb, (void *) &slsvmspace, sizeof(slsvmspace));
	if (error != 0)
	    return ENOMEM;


	/* Checkpoint all objects, including their ancestors. */
	for (entry = vm_map->header.next; entry != &vm_map->header; 
		entry = entry->next) {

	    /* SYSV shared memory is a special case. */
	    for (i = 0; i < shminfo.shmseg; i++) {
		if (entry->start == p->p_vmspace->vm_shm[i].va)
		    continue;
	    }

	    for (obj = entry->object.vm_object; obj != NULL; 
		    obj = obj->backing_object) {

		error = sls_vmobject_ckpt(p, obj);
		if (error != 0) 
		    return error;
	    }
	}

	for (entry = vm_map->header.next; entry != &vm_map->header;
		entry = entry->next) {

	    /* SYSV shared memory is a special case. */
	    for (i = 0; i < shminfo.shmseg; i++) {
		if (entry->start == p->p_vmspace->vm_shm[i].va)
		    continue;

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
void
sls_shadow(vm_object_t shadow, vm_object_t source, vm_ooffset_t offset)
{
	/*
	 * Store the offset into the source object, and fix up the offset into
	 * the new object.
	 */
	shadow->backing_object = source;
	shadow->backing_object_offset = offset;


	/* 
	* If this is the first shadow, then we transfer the reference
	* from the caller to the shadow, as done in vm_object_shadow.
	* Otherwise we add a reference to the shadow.
	*/
	if (source->shadow_count != 0)
	    source->ref_count += 1;

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

static void
sls_data_rest(vm_object_t object, struct slsdata *slsdata)
{
	struct slspagerun *pagerun; 
	vm_page_t page;
	char *data;
	int i;

	VM_OBJECT_WLOCK(object);

	LIST_FOREACH(pagerun, slsdata, next) {
	    /* XXX Do one page_alloc per pagerun */
	    for (i = 0; i < (pagerun->len >> PAGE_SHIFT); i++) {
		page = vm_page_alloc(object, pagerun->idx + i, VM_ALLOC_NORMAL);
		if (page == NULL) {
		    VM_OBJECT_WUNLOCK(object);
		    return;
		}

		page->valid = VM_PAGE_BITS_ALL;

		/* Copy the data over to the VM Object's pages */
		data = (char *) pmap_map(NULL, page->phys_addr,
			page->phys_addr + PAGE_SIZE,
			VM_PROT_READ | VM_PROT_WRITE);

		memcpy(data, &((char *) pagerun->data)[i << PAGE_SHIFT], PAGE_SIZE);

		vm_page_xunbusy(page);
	    }
	}

	VM_OBJECT_WUNLOCK(object);
}

int
sls_vmobject_rest(struct vm_object_info *info, struct slskv_table *objtable, 
	struct slsdata *slsdata)
{
	vm_object_t object;
	struct vnode *vp;
	int error;

	switch (info->type) {
	case OBJT_DEFAULT:

	    /* Simple vm_allocate*/
	    object = vm_object_allocate(OBJT_DEFAULT, info->size);

	    /* Restore any data pages the object might have. */
	    sls_data_rest(object, slsdata);

	    break;

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
	    break;

	/* 
	 * Device files either can't be mmap()'d, or have an mmap
	 * that maps anonymous objects, if they have the D_MMAP_ANON
	 * flag. In any case, we will never shadow an OBJT_DEVICE
	 * object, so we don't need it.
	 */
	case OBJT_DEVICE:
	    object = NULL;
	    break;
	
	/* 
	 * Physical objects are unlikely to back other objects, but we
	 * play it safe. The only way it could happen would be if 
	 * a physical object had a VM_INHERIT_COPY inheritance flag.
	 */
	case OBJT_PHYS:
	    object = curthread->td_proc->p_sysent->sv_shared_page_obj;
	    break;

	default:
	    return EINVAL;
	}

	/* Export the newly created/found object to the table. */
	error = slskv_add(objtable, info->slsid, (uintptr_t) object);
	if (error != 0)
	    return error;


	return 0;
}

/*
 * Variant of vm_map_insert, where we set the fields directly. 
 * Great for anonymous pages.
 */
static int
sls_vmentry_rest_anon(struct vm_map *map, struct vm_map_entry_info *info, struct slskv_table *objtable)
{
	vm_map_entry_t entry; 
	vm_page_t page;
	vm_offset_t vaddr;
	boolean_t contained;
	vm_object_t object;
	int error;

	/* Find an object if it exists. */
	if (slskv_find(objtable, (uint64_t) info->obj, (uintptr_t *) &object) != 0)
	    object = NULL;

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
	error = vm_map_insert(map, object, info->offset, 
		    info->start, info->end, 
		    info->protection, info->max_protection, 0);
	if (error != 0)
	    return error;

	/* Get the entry from the map. */
	contained = vm_map_lookup_entry(map, info->start, &entry);
	if (contained == FALSE)
	    return EINVAL;

	entry->eflags = info->eflags;
	entry->inheritance = info->inheritance;

	if (object != NULL) {
	    /* Enter the object's pages to the map's pmap. */
	    TAILQ_FOREACH(page, &object->memq, listq) {

		/* Compute the virtual address for the page. */
		vaddr = IDX_TO_VADDR(page->pindex, entry->start, entry->offset);

		vm_page_sbusy(page);

		/* If the page is within the entry's bounds, add it to the map. */
		error = pmap_enter(vm_map_pmap(map), vaddr, page, 
			entry->protection, VM_PROT_READ, page->psind);
		if (error != 0)
		    printf("Error: pmap_enter_failed\n");

		vm_page_sunbusy(page);
	    }
	}


	/* XXX restore cred if needed */
	entry->cred = NULL;
	vm_map_unlock(map);

	return 0;

}

static int
sls_vmentry_rest_file(struct vm_map *map, struct vm_map_entry_info *entry, struct slskv_table *objtable)
{
	vm_object_t object; 
	struct vnode *vp;
	struct sbuf *sb = NULL;
	int fd = -1;
	int flags;
	int rprot;
	int error;

	/* If we have an object, it has to have been restored. */
	error = slskv_find(objtable, (uint64_t) entry->obj, (uintptr_t *) &object);
	if (error != 0)
	    return error;

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
sls_vmentry_rest(struct vm_map *map, struct vm_map_entry_info *entry, struct slskv_table *objtable)
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
sls_vmspace_rest(struct proc *p, struct slsvmspace *slsvmspace)
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
	vmspace->vm_swrss = slsvmspace->vm_swrss;
	vmspace->vm_tsize = slsvmspace->vm_tsize;
	vmspace->vm_dsize = slsvmspace->vm_dsize;
	vmspace->vm_ssize = slsvmspace->vm_ssize;
	vmspace->vm_taddr = slsvmspace->vm_taddr;
	vmspace->vm_taddr = slsvmspace->vm_daddr;
	vmspace->vm_maxsaddr = slsvmspace->vm_maxsaddr; 

	return 0;
}
