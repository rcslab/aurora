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

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_mm.h"
#include "sls_path.h"
#include "sls_table.h"
#include "sls_vmobject.h"

int 
slsckpt_vmobject(struct proc *p, vm_object_t obj)
{
	struct slsvmobject cur_obj;
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
	cur_obj.magic = SLSVMOBJECT_ID;
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

static void
slsrest_data(vm_object_t object, struct slsdata *slsdata)
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
slsrest_vmobject(struct slsvmobject *info, struct slskv_table *objtable, 
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
	    slsrest_data(object, slsdata);

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

