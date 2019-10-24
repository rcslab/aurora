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


/*
 * Create a new struct slspart to be entered into the SLS.
 */
static int 
slsp_init(pid_t pid, struct slspart **slspp)
{
	struct slspart *procnew;

	/* Create the new process to */
	procnew = malloc(sizeof(*procnew), M_SLSMM, M_WAITOK | M_ZERO);
	procnew->slsp_pid = pid;
	procnew->slsp_status = 0;
	procnew->slsp_epoch = 0;
	procnew->slsp_refcount = 1;
	/* This pointer is set during checkpointing. */
	procnew->slsp_objects = NULL;
	
	*slspp = procnew;

	return 0;
}

/*
 * Add the running process with PID pid to the SLS. If a process 
 * of that PID is already in the SLS, the operation fails.
 */
struct slspart *
slsp_add(pid_t pid)
{
	struct slspart *slsp;
	int error;

	/* 
	* Try to find if we already have added the process 
	* to the SLS, if so we can't add it again.
	*/
	slsp = slsp_find(pid);
	if (slsp != NULL) {
	    /* We got a reference to the process with slsp_find, release it. */
	    slsp_deref(slsp);
	    return NULL;
	}

	/* If we didn't find it,. create one. */
	error = slsp_init(pid, &slsp);
	if (error != 0)
	    return NULL;

	/* Add the process to the hashtable. */
	error = slskv_add(slsm.slsm_proctable, pid, (uintptr_t) slsp);
	if (error != 0) {
	    slsp_fini(slsp);
	    return NULL;
	}

	return slsp;
}

/* 
 * Destroy a struct slspart. The struct has to have 
 * have already been removed from the hashtable.
 */
void
slsp_fini(struct slspart *slsp)
{
	vm_object_t obj, shadow;

	/* Remove any references to VM objects we may have. */
	if (slsp->slsp_objects != NULL) {

	    /* Collapse all shadows that we created. */
	    while (slskv_pop(slsp->slsp_objects, (uint64_t *) &obj, (uintptr_t *) &shadow) == 0)
		vm_object_deallocate(obj);

	    slskv_destroy(slsp->slsp_objects);
	}

	free(slsp, M_SLSMM);
}

/* Attempt to find and delete a process with PID pid from the SLS. */
void
slsp_del(pid_t pid)
{
	struct slspart *slsp;

	if (slskv_find(slsm.slsm_proctable, pid, (uintptr_t *) &slsp) != 0)
	    return;

	slskv_del(slsm.slsm_proctable, pid);
	slsp_fini(slsp);
}


/* Go through the process table of the SLS, listing all processes in it. */
static void
slsp_list(void)
{
	struct slspart *slsp;
	struct slskv_iter iter;
	uint64_t pid;

	iter = slskv_iterstart(slsm.slsm_proctable);
	while (slskv_itercont(&iter, &pid, (uintptr_t *) &slsp) != SLSKV_ITERDONE)
	    printf("PID %ld\n", pid);
}

/* Find a process in the SLS with the given PID. */
struct slspart *
slsp_find(pid_t pid)
{
	struct slspart *slsp;

	if (slskv_find(slsm.slsm_proctable, pid, (uintptr_t *) &slsp) != 0)
	    return NULL;

	/* We found the process, take a reference to it. */
	slsp_ref(slsp);

	return slsp;
}

/* Empty the SLS of all processes. */
void
slsp_delall(void)
{
	struct slspart *slsp;
	uint64_t pid;

	/* If we never completed initialization, abort. */
	if (slsm.slsm_proctable == NULL)
	    return;

	/* Remove all proces*/
	while (slskv_pop(slsm.slsm_proctable, &pid, (uintptr_t *) &slsp) == 0)
	    slsp_fini(slsp);
}

void
slsp_ref(struct slspart *slsp)
{
	atomic_add_int(&slsp->slsp_refcount, 1);
}

void
slsp_deref(struct slspart *slsp)
{
	atomic_add_int(&slsp->slsp_refcount, -1);

	if (slsp->slsp_refcount == 0)
	    slsp_del(slsp->slsp_pid);

}
