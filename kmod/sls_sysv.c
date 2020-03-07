#include <sys/types.h>

#include <sys/shm.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

#include <slos.h>
#include <slos_record.h>
#include <sls_data.h>

#include "imported_sls.h"
#include "sls_internal.h"
#include "sls_sysv.h"
/* 
 * Shadow and checkpoint all shared objects in the system. We assume all
 * shared objects are in use by workloads in the SLS, and so traverse the
 * whole space looking for valid segments.
 */
int
slsckpt_sysvshm(struct slsckpt_data *sckpt_data, struct slskv_table *objtable)
{
	struct slssysvshm slssysvshm;
	vm_object_t obj, shadow;
	struct sls_record *rec;
	struct sbuf *sb = NULL;
	vm_ooffset_t offset;
	int error, i;

	for (i = 0; i < shmalloced; i++) {
	    if ((shmsegs[i].u.shm_perm.mode & SHMSEG_ALLOCATED) == 0)
		continue;

	    /* Allocate an sbuf if we haven't already. */
	    if (sb == NULL)
		sb = sbuf_new_auto();

	    /* Dump the metadata to the records table. */
	    slssysvshm.magic = SLSSYSVSHM_ID;
	    slssysvshm.slsid = (uint64_t) shmsegs[i].object;
	    slssysvshm.key = shmsegs[i].u.shm_perm.key;
	    slssysvshm.shm_segsz = shmsegs[i].u.shm_segsz;
	    slssysvshm.mode = shmsegs[i].u.shm_perm.mode;
	    slssysvshm.segnum = i;

	    error = sbuf_bcat(sb, (void *) &slssysvshm, sizeof(slssysvshm));
	    if (error != 0)
		goto error;

	    /* If we have already shadowed, we can just mend the reference. */
	    error = slskv_find(objtable, (uint64_t) obj, (uintptr_t *) &shadow);
	    if (error == 0) {
		vm_object_reference(shadow);
		shmsegs[i].object = shadow;
		vm_object_deallocate(obj);
	    } else {
		/* Shadow the object and add it to the table. */
		obj = shmsegs[i].object;
		vm_object_reference(obj);

		offset = 0;
		vm_object_shadow(&shmsegs[i].object, &offset, ptoa(obj->size));

		/* It's impossible for us to have checkpointed the object yet. */
		error = slskv_add(objtable, (uint64_t) obj, (uintptr_t) shmsegs[i].object);
		KASSERT((error == 0), ("VM object already checkpointed"));
	    }

	}

	/* If we have no SYSV segments, don't store any data at all. */
	if (sb == NULL)
	    return (0);

	error = sbuf_finish(sb);
	if (error != 0)
	    goto error;

	rec = sls_getrecord(sb, (uint64_t) shmsegs, SLOSREC_SYSVSHM);
	error = slskv_add(sckpt_data->sckpt_rectable, (uint64_t) shmsegs, (uintptr_t) rec);
	if (error != 0) {
	    free(rec, M_SLSMM);
	    goto error;
	}

	return (0);
error:
	if (sb != NULL)
	    sbuf_delete(sb);

	return (error);
}


int
slsrest_sysvshm(struct slssysvshm *slssysvshm, struct slskv_table *objtable)
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
	shmseg = &shmsegs[slssysvshm->segnum];
	if ((shmseg->u.shm_perm.mode & SHMSEG_ALLOCATED) != 0)
	    return (EINVAL);
	
	/* Get the restored object for the segment. */
	error = slskv_find(objtable, slssysvshm->slsid, (uintptr_t *) &obj);
	if (error != 0)
	    return (EINVAL);

	/* 
	 * Recreate the segment, similarly to how it's done 
	 * in shmget_allocate_segment(). 
	 */

	shmseg->object = obj;
        shmseg->u.shm_perm.cuid = shmseg->u.shm_perm.uid = cred->cr_uid;
        shmseg->u.shm_perm.cgid = shmseg->u.shm_perm.gid = cred->cr_gid;
        shmseg->u.shm_perm.mode = (slssysvshm->mode & ACCESSPERMS) | SHMSEG_ALLOCATED;
        shmseg->u.shm_perm.key = slssysvshm->key;
        shmseg->u.shm_perm.seq = (shmseg->u.shm_perm.seq + 1) & 0x7fff;
        shmseg->cred = crhold(cred);
        shmseg->u.shm_segsz = slssysvshm->shm_segsz;
        shmseg->u.shm_cpid = curthread->td_proc->p_pid;
        shmseg->u.shm_lpid = shmseg->u.shm_nattch = 0;
        shmseg->u.shm_atime = shmseg->u.shm_dtime = 0;
        shmseg->u.shm_ctime = time_second;
        shm_committed += btoc(slssysvshm->shm_segsz);
        shm_nused++;

	printf("Restored\n");

	return (0);
}
