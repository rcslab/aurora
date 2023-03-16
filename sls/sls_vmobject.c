#include <sys/param.h>
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
#include <slos_inode.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_table.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vnode.h"

int
slsvmobj_checkpoint(vm_object_t obj, struct slsckpt_data *sckpt)
{
	vm_object_t curobj, backer;
	struct slsvmobject info;
	struct sbuf *sb;
	int error;

	/* Find if we have already checkpointed the object. */
	if (slskv_find(sckpt->sckpt_rectable, (uint64_t)obj->objid,
		(uintptr_t *)&sb) == 0)
		return (0);

	/* We don't need the anonymous objects for in-memory checkpointing. */
	if ((sckpt->sckpt_attr.attr_target == SLS_MEM) && OBJT_ISANONYMOUS(obj))
		return (0);

	DEBUG3("Checkpointing metadata for object %p (ID %lx, type %d)", obj,
	    obj->objid, obj->type);
	/* First time we come across it, create a buffer for the info struct. */
	sb = sbuf_new_auto();

	info.size = obj->size;
	info.type = obj->type;
	info.objptr = NULL;

	/*
	 * Get a reference for anonymous objects we are flushing out to the OSD.
	 * We remove the reference after we are done creating the IOs.
	 */
	if (OBJT_ISANONYMOUS(obj)) {
		vm_object_reference(obj);
		info.objptr = obj;
	}

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

	info.backer = (backer != NULL) ? backer->objid : 0UL;
	info.backer_off = (backer != NULL) ? obj->backing_object_offset : 0;
	info.magic = SLSVMOBJECT_ID;
	info.slsid = obj->objid;
	if (obj->type == OBJT_VNODE) {
		error = slsckpt_vnode((struct vnode *)obj->handle, sckpt);
		if (error != 0)
			goto error;
		info.vnode = (uint64_t)obj->handle;
	}

	error = sbuf_bcat(sb, (void *)&info, sizeof(info));
	if (error != 0)
		goto error;

	error = sbuf_finish(sb);
	if (error != 0)
		goto error;

	info.backer = (backer != NULL) ? backer->objid : 0UL;
	KASSERT((info.type != OBJT_DEVICE) || (info.backer == 0),
	    ("device object has a backer"));
	KASSERT(info.slsid != 0, ("object has an ID of 0"));

	error = slsckpt_addrecord(sckpt, info.slsid, sb, SLOSREC_VMOBJ);
	if (error != 0)
		goto error;

	return (0);

error:
	vm_object_deallocate(obj);
	slskv_del(sckpt->sckpt_rectable, (uint64_t)info.slsid);
	sbuf_delete(sb);

	return (error);
}

/*
 * Checkpoint and shadow a VM object backing a POSIX or SYSV memory segment.
 */
int
slsvmobj_checkpoint_shm(vm_object_t *objp, struct slsckpt_data *sckpt_data)
{
	vm_object_t obj, shadow;
	int error;

	obj = *objp;

	/* Lookup whether we have already shadowed the object. */
	error = slskv_find(
	    sckpt_data->sckpt_shadowtable, (uint64_t)obj, (uintptr_t *)&shadow);

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
		KASSERT(shadow != NULL,
		    ("shared object is in Aurora without a shadow"));

		VM_OBJECT_WLOCK(shadow);
		slsvm_object_reftransfer(obj, shadow);
		vm_object_clear_flag(shadow, OBJ_ONEMAPPING);
		VM_OBJECT_WUNLOCK(shadow);
	} else {
		/* Checkpoint the object. */
		error = slsvmobj_checkpoint(obj, sckpt_data);
		if (error != 0)
			return (error);

		/* Actually shadow it. */
		shadow = obj;
		error = slsvm_object_shadow(
		    sckpt_data->sckpt_shadowtable, &shadow);
		if (error != 0)
			return (error);
	}

	*objp = shadow;
	return (0);
}
static int
slsvmobj_restore(struct slsvmobject *info, struct slsckpt_data *sckpt,
    struct slskv_table *objtable)
{
	vm_object_t object;
	struct vnode *vp;
	int error;

	DEBUG2("(Object 0x%lx) type %d", info->slsid, info->type);
	switch (info->type) {
	case OBJT_DEFAULT:
		/* FALLTHROUGH */
	case OBJT_SWAP:

#ifdef INVARIANTS
		error = slskv_find(objtable, info->slsid, (uintptr_t *)&object);
		KASSERT(error == 0, ("object %lx not found", info->slsid));
#endif /* INVARIANTS */

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
		error = slskv_find(
		    sckpt->sckpt_vntable, info->vnode, (uintptr_t *)&vp);
		if (error != 0) {
			printf("FAILED\n");
			return (error);
		}

		if (sckpt->sckpt_attr.attr_target == SLS_OSD)
			slspre_vnode(vp, sckpt->sckpt_attr);

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
	error = slskv_add(objtable, info->slsid, (uintptr_t)object);
	if (error != 0)
		return error;

	return (0);
}

static int
slsvmobj_deserialize(struct slsvmobject *obj, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(obj, sizeof(*obj), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (obj->magic != SLSVMOBJECT_ID) {
		SLS_DBG(
		    "magic mismatch %lx vs %x\n", obj->magic, SLSVMOBJECT_ID);
		return (EINVAL);
	}

	return (0);
}

int
slsvmobj_restore_all(struct slsckpt_data *sckpt, struct slskv_table *objtable)
{
	struct slsvmobject info, *infop;
	vm_object_t parent, object;
	struct slskv_iter iter;
	struct sls_record *rec;
	uint64_t slsid;
	size_t buflen;
	char *buf;
	int error;

	/* First pass; create of find all objects to be used. */
	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{
		buf = (char *)sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		if (rec->srec_type != SLOSREC_VMOBJ)
			continue;

		/* Get the data associated with the object in the table. */
		error = slsvmobj_deserialize(&info, &buf, &buflen);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		/* Restore the object. */
		error = slsvmobj_restore(&info, sckpt, objtable);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}
	}

	/* Second pass; link up the objects to their shadows. */
	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{

		if (rec->srec_type != SLOSREC_VMOBJ)
			continue;

		/*
		 * Get the object restored for the given info struct.
		 * We take advantage of the fact that the VM object info
		 * struct is the first thing in the record to typecast
		 * the latter into the former, skipping the parse function.
		 */
		infop = (struct slsvmobject *)sbuf_data(rec->srec_sb);
		if (infop->backer == 0)
			continue;

		error = slskv_find(
		    objtable, infop->slsid, (uintptr_t *)&object);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		if (object == NULL)
			continue;

		/* Find a parent for the restored object, if it exists. */
		error = slskv_find(
		    objtable, infop->backer, (uintptr_t *)&parent);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		vm_object_reference(parent);
		slsvm_forceshadow(object, parent, infop->backer_off);
	}
	SLS_DBG("Restoration of VM Objects\n");

	return (0);
}
