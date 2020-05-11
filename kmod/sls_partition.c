#include <sys/types.h>

#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

#include <machine/reg.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <slos.h>
#include <slos_inode.h>
#include <slos_record.h>
#include <sls_data.h>

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_partition.h"

int
slsckpt_create(struct slsckpt_data **sckpt_datap)
{
	struct slsckpt_data *sckpt_data = NULL;
	int error;

	sckpt_data = malloc(sizeof(*sckpt_data), M_SLSMM, M_WAITOK | M_ZERO);

	error = slskv_create(&sckpt_data->sckpt_rectable);
	if (error != 0)
	    goto error;

	error = slskv_create(&sckpt_data->sckpt_objtable);
	if (error != 0)
	    goto error;

	*sckpt_datap = sckpt_data;

	return (0);

error:

	if (sckpt_data != NULL) {
	    slskv_destroy(sckpt_data->sckpt_objtable);
	    slskv_destroy(sckpt_data->sckpt_rectable);
	}

	free(sckpt_data, M_SLSMM);

	return (error);
}

void
slsckpt_destroy(struct slsckpt_data *sckpt_data)
{
	struct sls_record *rec;
	uint64_t slsid;

	if (sckpt_data == NULL)
	    return;

	if (sckpt_data->sckpt_objtable != NULL) {
	    slsvm_objtable_collapse(sckpt_data->sckpt_objtable);
	    slskv_destroy(sckpt_data->sckpt_objtable);
	}

	if (sckpt_data->sckpt_objtable != NULL) {
	    KV_FOREACH_POP(sckpt_data->sckpt_rectable, slsid, rec) {
		sbuf_delete(rec->srec_sb);
		free(rec, M_SLSMM);
	    }

	    slskv_destroy(sckpt_data->sckpt_rectable);
	}

	free(sckpt_data, M_SLSMM);
}

/* Find a process in the SLS with the given PID. */
struct slspart *
slsp_find(uint64_t oid)
{
	struct slspart *slsp;

	/* XXX LOCKING - See slsp_deref */
	/* Get the partition if it exists. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *) &slsp) != 0)
	    return (NULL);

	/* We found the process, take a reference to it. */
	slsp_ref(slsp);

	return slsp;
}

/* Attach a process to the partition. */
int
slsp_attach(uint64_t oid, pid_t pid)
{
	struct slspart *slsp;
	uintptr_t oldoid;
	int error;

	/* Make sure the PID isn't in the SLS already. */
	if (slskv_find(slsm.slsm_procs, pid, &oldoid) == 0)
	    return (EINVAL);

	/* Make sure the partition actually exists. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *) &slsp) != 0)
	    return (EINVAL);

	error = slskv_add(slsm.slsm_procs, pid, (uintptr_t) oid);
	KASSERT(error == 0, ("PID already in the SLS"));

	error = slsset_add(slsp->slsp_procs, pid);
	KASSERT(error == 0, ("PID already in the partition"));

	slsp->slsp_procnum += 1;
	
	return (0);
}

/* Detach a process from the partition. */
int
slsp_detach(uint64_t oid, pid_t pid)
{
	struct slspart *slsp;

	/* Make sure the partition actually exists. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *) &slsp) != 0)
	    return (EINVAL);

	/* Remove the process from both the partition and the SLS. */
	slskv_del(slsm.slsm_procs, pid);
	slsset_del(slsp->slsp_procs, pid);

	slsp->slsp_procnum -= 1;
	
	return (0);
}

/* Empty the partition of all processes. */
static int
slsp_detachall(struct slspart *slsp)
{
	uint64_t pid;

	/* Go through the PIDs resident in the partition, remove them. */
	KVSET_FOREACH_POP(slsp->slsp_procs, pid)
	    slskv_del(slsm.slsm_procs, pid);

	return (0);
}

/*
 * Create a new struct slspart to be entered into the SLS.
 */
static int 
slsp_init(uint64_t oid, struct sls_attr attr, struct slspart **slspp)
{
	struct slspart *slsp = NULL;
	int error;

	/* Create the new partition. */
	slsp = malloc(sizeof(*slsp), M_SLSMM, M_WAITOK | M_ZERO);
	slsp->slsp_oid = oid;
	slsp->slsp_attr = attr;
	/* The SLS module itself holds one reference to the partition. */
	slsp->slsp_refcount = 1;
	slsp->slsp_status = SPROC_AVAILABLE;
	slsp->slsp_epoch = SPROC_EPOCHINIT;

	/* Create the set of held processes. */
	error = slsset_create(&slsp->slsp_procs);
	if (error != 0)
	    goto error;

	*slspp = slsp;

	return (0);

error:
	if (slsp != NULL && slsp->slsp_procs != NULL)
	    slsset_destroy(slsp->slsp_procs);

	free(slsp, M_SLSMM);

	return (error);
}

/* 
 * Destroy a partition. The struct has to have 
 * have already been removed from the hashtable.
 */
static void
slsp_fini(struct slspart *slsp)
{
	/* Remove all processes currently in the partition from the SLS. */
	slsp_detachall(slsp);

	/* XXX TEMP Remove the partition from the SLOS. */
	slos_iremove(&slos, slsp->slsp_oid);

	/* Destroy the proc bookkeeping structure. */
	slsset_destroy(slsp->slsp_procs);

	/* Remove any references to VM objects we may have. */
	if (slsp->slsp_objects != NULL) {
	    slsvm_objtable_collapse(slsp->slsp_objects);
	    slskv_destroy(slsp->slsp_objects);
	}

	if (slsp->slsp_sckpt != NULL)
	    slsckpt_destroy(slsp->slsp_sckpt);

	free(slsp, M_SLSMM);
}

/* Add a partition with unique ID oid to the SLS. */
int
slsp_add(uint64_t oid, struct sls_attr attr, struct slspart **slspp)
{
	struct slspart *slsp;
	int error;

	/* 
	 * Try to find if we already have added the process 
	 * to the SLS, if so we can't add it again.
	 */
	slsp = slsp_find(oid);
	if (slsp != NULL) {
	    /* We got a reference to the process with slsp_find, release it. */

	    slsp_deref(slsp);
	    return (EINVAL);
	}

	/* If we didn't find it, create one. */
	error = slsp_init(oid, attr, &slsp);
	if (error != 0)
	    return (error);

	/* Add the partition to the table. */
	error = slskv_add(slsm.slsm_parts, oid, (uintptr_t) slsp);
	if (error != 0) {
	    slsp_fini(slsp);
	    return (error);
	}

	/* Export the partition to the caller. */
	*slspp = slsp;

	return (0);
}

/* Destroy a partition. */
void
slsp_del(uint64_t oid)
{
	struct slspart *slsp;

	/* If the partition doesn't actually exist, we're done. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *) &slsp) != 0)
	    return;

	/* Remove the process from the table, and destroy the struct itself. */
	slskv_del(slsm.slsm_parts, oid);
	slsp_fini(slsp);
}


/* Empty the SLS of all processes. */
void
slsp_delall(void)
{
	struct slspart *slsp;
	uint64_t oid;

	/* If we never completed initialization, abort. */
	if (slsm.slsm_parts == NULL)
	    return;

	/* Destroy all partitions. */
	while (slskv_pop(slsm.slsm_parts, &oid, (uintptr_t *) &slsp) == 0)
	    slsp_fini(slsp);

	/* Remove all processes from the global table.  */
	slskv_destroy(slsm.slsm_procs);
}

void
slsp_ref(struct slspart *slsp)
{
	atomic_add_int(&slsp->slsp_refcount, 1);
}

void
slsp_deref(struct slspart *slsp)
{
	/* 
	 * XXX LOCKING - There currently is a race condition,
	 * because we an slsp with refcount 0 is still accessible
	 * via the hashtable. We need a lock to mediate between
	 * slsp_deref() and slsp_find().
	 */
	atomic_add_int(&slsp->slsp_refcount, -1);

	if (slsp->slsp_refcount == 0)
	    slsp_del(slsp->slsp_oid);

}

int
slsp_isempty(struct slspart *slsp)
{
	return (slsp->slsp_procnum == 0);
}

void
slsp_epoch_advance(struct slspart *slsp)
{
	slsp->slsp_epoch += 1;
}
