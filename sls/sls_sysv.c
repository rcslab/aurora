#include <sys/types.h>
#include <sys/shm.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

#include <slos.h>
#include <slos_inode.h>
#include <sls_data.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_sysv.h"
#include "sls_vmobject.h"
#include "sysv_internal.h"

/*
 * Shadow and checkpoint all shared objects in the system. We assume all
 * shared objects are in use by workloads in the SLS, and so traverse the
 * whole space looking for valid segments.
 */

int
slsckpt_sysvshm(struct slsckpt_data *sckpt)
{
	struct slssysvshm slssysvshm;
	struct sbuf *sb = NULL;
	int error, i;

	DEBUG("Checkpointing SYSV shared memory");

	for (i = 0; i < shmalloced; i++) {
		if ((shmsegs[i].u.shm_perm.mode & SHMSEG_ALLOCATED) == 0)
			continue;

		/* Allocate an sbuf if we haven't already. */
		if (sb == NULL)
			sb = sbuf_new_auto();

		/* Dump the metadata to the records table. */
		slssysvshm.magic = SLSSYSVSHM_ID;
		slssysvshm.slsid = (uint64_t)shmsegs[i].object->objid;
		slssysvshm.key = shmsegs[i].u.shm_perm.key;
		slssysvshm.shm_segsz = shmsegs[i].u.shm_segsz;
		slssysvshm.mode = shmsegs[i].u.shm_perm.mode;
		slssysvshm.seq = shmsegs[i].u.shm_perm.seq;
		slssysvshm.segnum = i;

		error = sbuf_bcat(sb, (void *)&slssysvshm, sizeof(slssysvshm));
		if (error != 0)
			goto error;

		KASSERT(shmsegs[i].object != NULL, ("segment has no object"));
		error = slsvmobj_checkpoint_shm(&shmsegs[i].object, sckpt);
		if (error != 0)
			goto error;
	}

	/* If we have no SYSV segments, don't store any data at all. */
	if (sb == NULL)
		return (0);

	error = sbuf_finish(sb);
	if (error != 0)
		goto error;

	error = slsckpt_addrecord(
	    sckpt, (uint64_t)shmsegs, sb, SLOSREC_SYSVSHM);

error:
	if (error != 0 && sb != NULL)
		sbuf_delete(sb);

	return (error);
}

int
slsrest_sysvshm(struct slssysvshm *info, struct slskv_table *objtable)
{
	struct ucred *cred = curthread->td_ucred;
	struct shmid_kernel *shmseg;
	vm_object_t obj;
	int error;

	/*
	 * The segments have to have the exact same segment number
	 * they originally used to have when restored. XXX We could
	 * fix that up by having a translation table, but having a
	 * clean slate to work with shared memory-wise is a reasonable
	 * assumption.
	 */
	KASSERT(shmalloced > info->segnum,
	    ("shmalloced %d, segnum %d", shmalloced, info->segnum));
	shmseg = &shmsegs[info->segnum];
	if ((shmseg->u.shm_perm.mode & SHMSEG_ALLOCATED) != 0)
		return (EINVAL);

	/* Get the restored object for the segment. */
	error = slskv_find(objtable, info->slsid, (uintptr_t *)&obj);
	if (error != 0)
		return (EINVAL);

	/*
	 * Recreate the segment, similarly to how it's done
	 * in shmget_allocate_segment().
	 */

	vm_object_reference(obj);
	shmseg->object = obj;
	shmseg->u.shm_perm.cuid = shmseg->u.shm_perm.uid = cred->cr_uid;
	shmseg->u.shm_perm.cgid = shmseg->u.shm_perm.gid = cred->cr_gid;
	shmseg->u.shm_perm.mode = (info->mode & ACCESSPERMS) | SHMSEG_ALLOCATED;
	shmseg->u.shm_perm.key = info->key;
	shmseg->u.shm_perm.seq = info->seq;
	shmseg->cred = crhold(cred);
	shmseg->u.shm_segsz = info->shm_segsz;
	shmseg->u.shm_cpid = curthread->td_proc->p_pid;
	shmseg->u.shm_lpid = shmseg->u.shm_nattch = 0;
	shmseg->u.shm_atime = shmseg->u.shm_dtime = 0;
	shmseg->u.shm_ctime = time_second;
	shm_committed += btoc(info->shm_segsz);
	shm_nused++;

	return (0);
}
