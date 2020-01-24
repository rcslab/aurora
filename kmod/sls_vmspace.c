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

#include <slos.h>

#include "imported_sls.h"
#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_mm.h"
#include "sls_path.h"
#include "sls_table.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"

static int
slsckpt_vmentry(struct vm_map_entry *entry, struct sbuf *sb)
{
	struct slsvmentry cur_entry;
	int error;

	cur_entry.magic = SLSVMENTRY_ID;

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
	    return (ENOMEM);

	return (0);
}

int
slsckpt_vmspace(struct proc *p, struct sbuf *sb, long mode)
{
	vm_map_t vm_map;
	struct vmspace *vmspace;
	struct vm_map_entry *entry;
	struct slsvmspace slsvmspace;
	vm_object_t obj;
	int error;

	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;

	slsvmspace = (struct slsvmspace) {
	    .magic = SLSVMSPACE_ID,
	    .vm_swrss = vmspace->vm_swrss,
	    .vm_tsize = vmspace->vm_tsize,
	    .vm_dsize = vmspace->vm_dsize,
	    .vm_ssize = vmspace->vm_ssize,
	    .vm_taddr = vmspace->vm_taddr,
	    .vm_daddr = vmspace->vm_daddr,
	    .vm_maxsaddr = vmspace->vm_maxsaddr,
	    .nentries = vm_map->nentries,
	    .has_shm = ((vmspace->vm_shm != NULL) ? 1 : 0),
	};

	error = sbuf_bcat(sb, (void *) &slsvmspace, sizeof(slsvmspace));
	if (error != 0)
	    return (error);

	/* Get the table wholesale (it's not large, so just grab all of it). */
	if (slsvmspace.has_shm != 0) {
	    error = sbuf_bcat(sb, vmspace->vm_shm, shminfo.shmseg * sizeof(*vmspace->vm_shm));
	    if (error != 0)
		return (error);
	}

	/* Checkpoint all objects, including their ancestors. */
	for (entry = vm_map->header.next; entry != &vm_map->header; 
		entry = entry->next) {

	    for (obj = entry->object.vm_object; obj != NULL; 
		    obj = obj->backing_object) {

		error = slsckpt_vmobject(p, obj);
		if (error != 0) 
		    return (error);
	    }
	}

	for (entry = vm_map->header.next; entry != &vm_map->header;
		entry = entry->next) {
	    error = slsckpt_vmentry(entry, sb);
	    if (error != 0)
		return (error);
	}

	return (0);
}

/*
 * Variant of vm_map_insert, where we set the fields directly. 
 * Great for anonymous pages.
 */
static int
slsrest_vmentry_anon(struct vm_map *map, struct slsvmentry *info, struct slskv_table *objtable)
{
	vm_map_entry_t entry; 
	vm_page_t page;
	vm_offset_t vaddr;
	boolean_t contained;
	vm_object_t object;
	int guard;
	int error;

	/* Find an object if it exists. */
	if (slskv_find(objtable, (uint64_t) info->obj, (uintptr_t *) &object) != 0)
	    object = NULL;
	else
	    vm_object_reference(object);

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
	guard = (info->eflags & MAP_ENTRY_GUARD) ? MAP_CREATE_GUARD : 0;
	error = vm_map_insert(map, object, info->offset, 
		    info->start, info->end, 
		    info->protection, info->max_protection, guard);
	if (error != 0)
	    goto out;


	/* Get the entry from the map. */
	contained = vm_map_lookup_entry(map, info->start, &entry);
	if (contained == FALSE) {
	    error = EINVAL;
	    goto out;
	}

	entry->eflags = info->eflags;
	entry->inheritance = info->inheritance;

	if (object != NULL) {
	    /* Enter the object's pages to the map's pmap. */
	    TAILQ_FOREACH(page, &object->memq, listq) {

		/* Compute the virtual address for the page. */
		vaddr = IDX_TO_VADDR(page->pindex, entry->start, entry->offset);

		vm_page_sbusy(page);

		/* If the page is within the entry's bounds, add it to the map. */
		VM_OBJECT_WLOCK(object);
		error = pmap_enter(vm_map_pmap(map), vaddr, page, 
			entry->protection, VM_PROT_READ, page->psind);
		VM_OBJECT_WUNLOCK(object);
		if (error != 0)
		    printf("Error: pmap_enter_failed\n");

		vm_page_sunbusy(page);
	    }
	}


	/* XXX restore cred if needed */
	entry->cred = NULL;

out:
	vm_map_unlock(map);

	/* Go from Mach to Unix error codes. */
	error = vm_mmap_to_errno(error);
	return (error);
}

static int
slsrest_vmentry_file(struct vm_map *map, struct slsvmentry *entry, struct slskv_table *objtable)
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
	    return (error);

	vp = (struct vnode *) object->handle;
	SLS_DBG("vp %p\n", vp);
	error = sls_vn_to_path(vp, &sb);
	if (error != 0)
	    return (EINVAL);

	if (entry->protection == VM_PROT_NONE) {
	    error = EPERM;
	    goto done;
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
	    goto done;

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

	vref(vp);

	if (entry->eflags & MAP_ENTRY_VN_EXEC)
	    VOP_SET_TEXT(vp);

done:

	if (fd >= 0)
	    kern_close(curthread, fd);

	if (sb != NULL)
	    sbuf_delete(sb);

	return (error);
}


int
slsrest_vmentry(struct vm_map *map, struct slsvmentry *entry, struct slskv_table *objtable)
{
	int error;
	int fd;

	/* If it's a guard page use the code for anonymous/empty/physical entries. */
	if (entry->obj == NULL) 
	    return slsrest_vmentry_anon(map, entry, objtable);


	/* Jump table for restoring the entries. */
	switch (entry->type) {
	case OBJT_DEFAULT:
	case OBJT_SWAP:
	    return slsrest_vmentry_anon(map, entry, objtable);

	case OBJT_PHYS:
	    return slsrest_vmentry_anon(map, entry, objtable);

	case OBJT_VNODE:
	    return slsrest_vmentry_file(map, entry, objtable);

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
		return (error);

	    fd = curthread->td_retval[0];
	    error = kern_mmap(curthread, entry->start, entry->end - entry->start, 
		    entry->protection, MAP_SHARED, fd, entry->offset);

	    kern_close(curthread, fd);
	    return (error);

	default:
	    return (EINVAL);
	}
}

int
slsrest_vmspace(struct proc *p, struct slsvmspace *info, struct shmmap_state *shmstate)
{
	struct vmspace *vmspace;
	struct vm_map *vm_map;

	/* Shorthands */
	vmspace = p->p_vmspace;
	vm_map = &vmspace->vm_map;

	/* Blow away the old address space, as done in exec_new_vmspace. */
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
	vmspace->vm_swrss = info->vm_swrss;
	vmspace->vm_tsize = info->vm_tsize;
	vmspace->vm_dsize = info->vm_dsize;
	vmspace->vm_ssize = info->vm_ssize;
	vmspace->vm_taddr = info->vm_taddr;
	vmspace->vm_taddr = info->vm_daddr;
	vmspace->vm_maxsaddr = info->vm_maxsaddr; 

	if (shmstate != NULL)
	    vmspace->vm_shm = shmstate;

	return (0);
}
