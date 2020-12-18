#include <sys/param.h>

#include <machine/param.h>

#include <sys/conf.h>
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
#include <slos_inode.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_table.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vnode.h"

#include "debug.h"

int
slsckpt_vmobject(vm_object_t obj, struct slsckpt_data *sckpt_data)
{
	struct slsvmobject cur_obj;
	struct sls_record *rec;
	vm_object_t curobj, backer;
	struct sbuf *sb;
	int error;

	/* Find if we have already checkpointed the object. */
	if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) obj->objid, (uintptr_t *) &sb) == 0)
		return (0);

	/* We don't need the anonymous objects for in-memory checkpointing. */
	if ((sckpt_data->sckpt_attr.attr_target == SLS_MEM) && OBJT_ISANONYMOUS(obj))
		return (0);

	DEBUG3("Checkpointing metadata for object %p (ID %lx, type %d)", obj, obj->objid, obj->type);
	/* First time we come across it, create a buffer for the info struct. */
	sb = sbuf_new_auto();

	cur_obj.size = obj->size;
	cur_obj.type = obj->type;
	cur_obj.objptr = obj;

	/*
	 * If the backer has the same ID as we do, we're an Aurora shadow. Find
	 * the first non-Aurora ancestor.
	 */
	curobj = obj;
	backer = obj->backing_object;
	while ((backer != NULL) && (backer->objid == curobj->objid)) {
		curobj = backer;
		backer = backer->backing_object;
	}

	cur_obj.backer = (backer != NULL) ? backer->objid : 0UL;
	cur_obj.backer_off = (backer != NULL) ? obj->backing_object_offset : 0;
	cur_obj.magic = SLSVMOBJECT_ID;
	cur_obj.slsid = obj->objid;
	if (obj->type == OBJT_VNODE) {
		error = slsckpt_vnode((struct vnode *) obj->handle, sckpt_data);
		if (error != 0)
			goto error;
		cur_obj.vnode = (uint64_t) obj->handle;
	}

	error = sbuf_bcat(sb, (void *) &cur_obj, sizeof(cur_obj));
	if (error != 0)
		goto error;

	error = sbuf_finish(sb);
	if (error != 0)
		goto error;

	cur_obj.backer = (backer != NULL) ? backer->objid : 0UL;
	KASSERT((cur_obj.type != OBJT_DEVICE) || (cur_obj.backer == 0), ("device object has a backer"));
	KASSERT(cur_obj.slsid != 0, ("object has an ID of 0"));

	rec = sls_getrecord(sb, cur_obj.slsid, SLOSREC_VMOBJ);

	error = slskv_add(sckpt_data->sckpt_rectable, (uint64_t) cur_obj.slsid, (uintptr_t) rec);
	if (error != 0) {
		free(rec, M_SLSREC);
		goto error;
	}

	return (0);

error:
	slskv_del(sckpt_data->sckpt_rectable, (uint64_t) cur_obj.slsid);
	sbuf_delete(sb);

	return (error);
}

/*
 * Checkpoint and shadow a VM object backing a POSIX or SYSV memory segment.
 */
int
slsckpt_vmobject_shm(vm_object_t *objp, struct slsckpt_data *sckpt_data)
{
	vm_object_t obj, shadow;
	int error;

	obj = *objp;

	/* Lookup whether we have already shadowed the object. */
	error = slskv_find(sckpt_data->sckpt_objtable, (uint64_t) obj, (uintptr_t *) &shadow);

	/*
	 * Either the VM object is already mapped, or it is not. If it
	 * is not, we need to shadow it ourselves and add it to the
	 * object table. If it is already checkpointed, we already have it on 
	 * the table, so we only need to fix the reference from the file handle.  
	 * In any case, we don't need to worry about physical mappings.
	 *
	 * We checkpoint the file mappings before shadowing, but the
	 * object might be open from multiple processes.
	 */
	if (error == 0) {
		KASSERT(shadow != NULL, ("shared object is in Aurora without a shadow"));

		VM_OBJECT_WLOCK(shadow);
		slsvm_object_reftransfer(obj, shadow);
		vm_object_clear_flag(shadow, OBJ_ONEMAPPING);
		VM_OBJECT_WUNLOCK(shadow);
	} else {
		/* Checkpoint the object. */
		error = slsckpt_vmobject(obj, sckpt_data);
		if (error != 0)
			return (error);

		/* Actually shadow it. */
		shadow = obj;
		error = slsvm_object_shadow(sckpt_data->sckpt_objtable, &shadow);
		if (error != 0)
			return (error);
	}

	*objp = shadow;
	return (0);
}


int
slsrest_vmobject(struct slsvmobject *info, struct slsrest_data *restdata)
{
	vm_object_t object;
	struct vnode *vp;
	int error;

	DEBUG2("(Object 0x%lx) type %d", info->slsid, info->type);
	switch (info->type) {
	case OBJT_DEFAULT:
		/* FALLTHROUGH */
	case OBJT_SWAP:
		/*
		 * OBJT_SWAP is just a default object which has swapped, or is 
		 * SYSV_SHM. It is already restored and set up.
		 */
		return (0);

	case OBJT_VNODE:
		/*
		 * Just grab the object directly, so that we can find it 
		 * and shadow it. We deal with objects that are directly mapped 
		 * into the address space, as in the case of executables, 
		 * in vmentry_rest.
		 */

		error = slskv_find(restdata->vntable, info->vnode, (uintptr_t *) &vp);
		if (error != 0)
			return (error);

		/*
		 * Get a reference for the vnode, since we're going to use it. 
		 * Do the same for the underlying object.
		 */
		object = vp->v_object;
		KASSERT(object != NULL, ("vnode is backed by NULL object"));
		vm_object_reference(object);
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
		vm_object_reference(object);
		break;

	default:
		return (EINVAL);
	}

	DEBUG2("Restored object for %lx is %p", info->slsid, object);
	/* Export the newly created/found object to the table. */
	error = slskv_add(restdata->objtable, info->slsid, (uintptr_t) object);
	if (error != 0)
		return error;

	return (0);
}

