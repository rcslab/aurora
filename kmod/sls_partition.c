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

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_partition.h"

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
	
	return (0);
}

/* Empty the partition of all processes. */
static int
slsp_detachall(struct slspart *slsp)
{
	uint64_t pid;

	/* Go through the PIDs resident in the partition, remove them. */
	while (slsset_pop(slsp->slsp_procs, &pid) == 0)
	    slskv_del(slsm.slsm_procs, pid);

	return (0);
}

/*
 * Create a new struct slspart to be entered into the SLS.
 */
static int 
slsp_init(uint64_t oid, struct slspart **slspp)
{
	struct slspart *slsp;
	int error;

	/* Create the new partition. */
	slsp = malloc(sizeof(*slsp), M_SLSMM, M_WAITOK | M_ZERO);
	slsp->slsp_oid = oid;
	slsp->slsp_status = 0;
	slsp->slsp_epoch = 0;
	/* The SLS module itself holds one reference to the partition. */
	slsp->slsp_refcount = 1;
	/* This pointer is set during checkpointing. */
	slsp->slsp_objects = NULL;
	/* XXX slsp->slsp_attr? We should define the backend at creation time. */

	/* Create the set of held processes. */
	error = slsset_create(&slsp->slsp_procs);
	if (error != 0) {
	    free(slsp, M_SLSMM);
	    return (error);
	}
	
	*slspp = slsp;

	return (0);
}

/* 
 * Destroy a partition. The struct has to have 
 * have already been removed from the hashtable.
 */
static void
slsp_fini(struct slspart *slsp)
{
	vm_object_t obj, shadow;

	/* Remove all processes currently in the partition from the SLS. */
	slsp_detachall(slsp);

	/* Destroy the proc bookkeeping structure. */
	slsset_destroy(slsp->slsp_procs);

	/* Remove any references to VM objects we may have. */
	if (slsp->slsp_objects != NULL) {

	    /* Collapse all shadows that we created. */
	    while (slskv_pop(slsp->slsp_objects, (uint64_t *) &obj, (uintptr_t *) &shadow) == 0)
		vm_object_deallocate(obj);

	    slskv_destroy(slsp->slsp_objects);
	}

	free(slsp, M_SLSMM);
}

/*
 * Add the running process with PID pid to the SLS. If a process 
 * of that PID is already in the SLS, the operation fails.
 */
int
slsp_add(uint64_t oid, struct slspart **slspp)
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
	error = slsp_init(oid, &slsp);
	if (error != 0)
	    return (error);

	/* Add the partition to the table. */
	error = slskv_add(slsm.slsm_parts, oid, (uintptr_t) slsp);
	if (error != 0) {
	    slsp_fini(slsp);
	    return (error);
	}

	printf("Added %ld - %p\n", oid, slsp);
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


/* Go through the process table of the SLS, listing all processes in it. */
static void
slsp_list(void)
{
	struct slskv_iter iter;
	uint64_t pid, oid;

	/* XXX Make it so it actually produces useful, partition-centric info. */
	iter = slskv_iterstart(slsm.slsm_procs);
	while (slskv_itercont(&iter, &pid, (uintptr_t *) &oid) != SLSKV_ITERDONE)
	    printf("PID %ld, OID %ld\n", pid, oid);
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
