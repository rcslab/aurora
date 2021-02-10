#include <sys/param.h>
#include <sys/conf.h>
#include <sys/exec.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/sysent.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h>
#include <vm/vm_radix.h>

#include <machine/param.h>

#include <slos.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_table.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"
#include "sysv_internal.h"

fo_mmap_t vn_mmap;

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
	cur_entry.slsid = (uint64_t)entry;
	if (entry->object.vm_object != NULL) {
		cur_entry.obj = entry->object.vm_object->objid;
		cur_entry.type = entry->object.vm_object->type;
		if (cur_entry.type == OBJT_VNODE)
			cur_entry.vp = (uint64_t)
					   entry->object.vm_object->handle;
	} else {
		cur_entry.obj = 0;
		cur_entry.type = OBJT_DEAD;
	}

	error = sbuf_bcat(sb, (void *)&cur_entry, sizeof(cur_entry));
	if (error != 0)
		return (ENOMEM);

	return (0);
}

int
slsckpt_vmspace(
    struct vmspace *vm, struct sbuf *sb, struct slsckpt_data *sckpt_data)
{
	vm_map_t vm_map = &vm->vm_map;
	struct vm_map_entry *entry;
	struct slsvmspace slsvmspace;
	vm_object_t obj;
	int error;

	slsvmspace = (struct slsvmspace) {
		.magic = SLSVMSPACE_ID,
		.vm_swrss = vm->vm_swrss,
		.vm_tsize = vm->vm_tsize,
		.vm_dsize = vm->vm_dsize,
		.vm_ssize = vm->vm_ssize,
		.vm_taddr = vm->vm_taddr,
		.vm_daddr = vm->vm_daddr,
		.vm_maxsaddr = vm->vm_maxsaddr,
		.nentries = vm_map->nentries,
		.has_shm = ((vm->vm_shm != NULL) ? 1 : 0),
	};

	error = sbuf_bcat(sb, (void *)&slsvmspace, sizeof(slsvmspace));
	if (error != 0)
		return (error);

	/* Get the table wholesale (it's not large, so just grab all of it). */
	if (slsvmspace.has_shm != 0) {
		error = sbuf_bcat(
		    sb, vm->vm_shm, shminfo.shmseg * sizeof(*vm->vm_shm));
		if (error != 0)
			return (error);
		KASSERT(
		    vm->vm_shm != NULL, ("shmmap state array was deallocated"));
	}

	/* Checkpoint all objects, including their ancestors. */
	for (entry = vm_map->header.next; entry != &vm_map->header;
	     entry = entry->next) {
		for (obj = entry->object.vm_object; obj != NULL;
		     obj = obj->backing_object) {
			error = slsckpt_vmobject(obj, sckpt_data);
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
slsrest_vmentry_anon(
    struct vm_map *map, struct slsvmentry *info, struct slskv_table *objtable)
{
	vm_map_entry_t entry;
	vm_object_t object;
	int guard;
	int error;

	/* Find an object if it exists. */
	if ((info->obj == 0) ||
	    slskv_find(objtable, (uint64_t)info->obj, (uintptr_t *)&object) !=
		0)
		object = NULL;
	else
		vm_object_reference(object);

	/*
	 * Create a map entry using and manually set the fields we can't set by
	 * using the proper arguments (mainly flags).
	 */
	vm_map_lock(map);
	guard = MAP_NO_MERGE;
	if (info->eflags & MAP_ENTRY_GUARD)
		guard |= MAP_CREATE_GUARD;

	error = vm_map_insert(map, object, info->offset, info->start, info->end,
	    info->protection, info->max_protection, guard);
	if (error != 0)
		goto out;

#ifdef INVARIANTS
	/* Get the entry from the map. */
	boolean_t contained = vm_map_lookup_entry(map, info->start, &entry);
	KASSERT(contained == TRUE, ("lost inserted vm_map_entry"));
	KASSERT(entry->start == info->start,
	    ("vm_map_entry start doesn't match: %lx vs. %lx", entry->start,
		info->start));
	KASSERT(entry->end == info->end,
	    ("vm_map_entry end doesn't match: %lx vs. %lx", entry->end,
		info->end));
#else
	vm_map_lookup_entry(map, info->start, &entry);
#endif /* INVARIANTS */

	entry->eflags = info->eflags;
	entry->inheritance = info->inheritance;

	/*
	 * Set the entry as text if it is backed by a vnode or an object
	 * shadowing a vnode.
	 */
	if (entry->eflags & MAP_ENTRY_VN_EXEC)
		vm_map_entry_set_vnode_text(entry, true);

out:
	vm_map_unlock(map);

	/* Go from Mach to Unix error codes. */
	error = vm_mmap_to_errno(error);
	return (error);
}

static int
slsrest_vmentry_file(
    struct vm_map *map, struct slsvmentry *entry, struct slsrest_data *restdata)
{
	struct thread *td = curthread;
	vm_offset_t start;
	struct vnode *vp;
	struct file *fp;
	int protection;
	int flags;
	int error;

	DEBUG2("Restoring file backed entry 0x%lx, (flags 0x%lx)", entry->start,
	    entry->eflags);
	if (entry->protection == VM_PROT_NONE)
		return (EPERM);

	/* Retrieve the restored vnode pointer. */
	error = slskv_find(
	    restdata->vntable, (uint64_t)entry->vp, (uintptr_t *)&vp);
	if (error != 0)
		return (error);

	/* Create a dummy fp, through which to pass the vp to vn_mmap(). */
	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (error);

	fp->f_vnode = vp;
	fp->f_flag = 0;
	if (entry->protection & VM_PROT_READ)
		fp->f_flag |= FREAD;
	if (entry->protection & VM_PROT_WRITE)
		fp->f_flag |= FWRITE;

	protection = entry->protection & VM_PROT_ALL;

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

	start = entry->start;

	KASSERT(vp->v_object->handle == vp,
	    ("vnode object is backed by another vnode"));
	error = vn_mmap(fp, map, &start, entry->end - entry->start,
	    entry->protection, entry->max_protection, flags, entry->offset, td);
	KASSERT(start == entry->start,
	    ("vn_mmap did not return requested address"));

	/*
	 * Modify the file ops so that that fo_close() is a no-op.
	 */
	fp->f_vnode = NULL;
	fp->f_flag = 0;
	fp->f_ops = &badfileops;
	fdrop(fp, td);

	/* Now that the fp is cleaned up, check if vn_mmap() succeeded. */
	if (error != 0)
		return (error);

	if (entry->eflags & MAP_ENTRY_VN_EXEC)
		VOP_SET_TEXT(vp);

	return (error);
}

int
slsrest_vmentry(
    struct vm_map *map, struct slsvmentry *entry, struct slsrest_data *restdata)
{
	int error;
	int fd;

	DEBUG2("Restoring entry 0x%lx (type %d)", entry->start, entry->type);

	/* If it's a guard page use the code for anonymous/empty/physical
	 * entries. */
	if (entry->obj == 0)
		return slsrest_vmentry_anon(map, entry, restdata->objtable);

	/* Jump table for restoring the entries. */
	switch (entry->type) {
	case OBJT_DEFAULT:
	case OBJT_SWAP:
		return slsrest_vmentry_anon(map, entry, restdata->objtable);

	case OBJT_PHYS:
		return slsrest_vmentry_anon(map, entry, restdata->objtable);

	case OBJT_VNODE:
		return slsrest_vmentry_file(map, entry, restdata);

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
		error = kern_mmap(curthread, entry->start,
		    entry->end - entry->start, entry->protection, MAP_SHARED,
		    fd, entry->offset);

		kern_close(curthread, fd);
		return (error);

	default:
		return (EINVAL);
	}
}

int
slsrest_vmspace(
    struct proc *p, struct slsvmspace *info, struct shmmap_state *shmstate)
{
	struct vmspace *vmspace;
	vm_offset_t sv_minuser;
	struct sysentvec *sv;
	vm_map_t map;
	int error;
	int map_at_zero;
	ssize_t size;

	/* Shorthands */
	vmspace = p->p_vmspace;
	map = &vmspace->vm_map;
	sv = p->p_sysent;

	size = sizeof(map_at_zero);
	error = kernel_sysctlbyname(curthread, "security.bsd.map_at_zero",
	    &map_at_zero, &size, NULL, 0, NULL, 0);

	/* Blow away the old address space, as done in exec_new_vmspace. */
	if (error == 0 && map_at_zero)
		sv_minuser = sv->sv_minuser;
	else
		sv_minuser = MAX(sv->sv_minuser, PAGE_SIZE);
	if (vmspace->vm_refcnt == 1 && vm_map_min(map) == sv_minuser &&
	    vm_map_max(map) == sv->sv_maxuser &&
	    cpu_exec_vmspace_reuse(p, map)) {

		shmexit(vmspace);
		pmap_remove_pages(vmspace_pmap(vmspace));
		vm_map_remove(map, vm_map_min(map), vm_map_max(map));
		/*
		 * An exec terminates mlockall(MCL_FUTURE), ASLR state
		 * must be re-evaluated.
		 */
		vm_map_lock(map);
		vm_map_modflags(
		    map, 0, MAP_WIREFUTURE | MAP_ASLR | MAP_ASLR_IGNSTART);
		vm_map_unlock(map);
	} else {
		error = vmspace_exec(p, sv_minuser, sv->sv_maxuser);
		if (error)
			return (error);
		vmspace = p->p_vmspace;
	}

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
