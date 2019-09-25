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

#include "sls.h"
#include "slsmm.h"
#include "sls_data.h"
#include "sls_process.h"


/*
 * Create a new struct sls_process to be entered into the SLS.
 */
static struct sls_process *
slsp_init(pid_t pid)
{
	struct sls_process *procnew;
	int error;

	/* Create the new process to */
	procnew = malloc(sizeof(*procnew), M_SLSMM, M_WAITOK);
	procnew->slsp_pid = pid;
	procnew->slsp_vm = NULL;
	procnew->slsp_charge = 0;
	procnew->slsp_status = 0;
	procnew->slsp_epoch = 0;
	procnew->slsp_ckptbuf = sbuf_new_auto();
	procnew->slsp_refcount = 1;
	if (procnew->slsp_ckptbuf == NULL) {
	    free(procnew, M_SLSMM);
	    return NULL;
	}
	
	/* Add the process to the hashtable. */
	error = slskv_add(slsm.slsm_proctable, pid, (uintptr_t) procnew);
	if (error != 0) {
	    slsp_fini(procnew);
	    return NULL;
	}

	return procnew;
}

/*
 * Add the running process with PID pid to the SLS. If a process 
 * of that PID is already in the SLS, the operation fails.
 */
struct sls_process *
slsp_add(pid_t pid)
{
	struct sls_process *slsp;

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

	return slsp_init(pid);
}

/* 
 * Destroy a struct sls_process. The struct has to have 
 * have already been removed from the hashtable.
 */
void
slsp_fini(struct sls_process *slsp)
{
	if (slsp->slsp_ckptbuf != NULL) {
	    if (sbuf_done(slsp->slsp_ckptbuf) == 0)
		sbuf_finish(slsp->slsp_ckptbuf);

	    sbuf_delete(slsp->slsp_ckptbuf);
	}

	if (slsp->slsp_vm != NULL)
	    vmspace_free(slsp->slsp_vm);
}

/* Attempt to find and delete a process with PID pid from the SLS. */
void
slsp_del(pid_t pid)
{
	struct sls_process *slsp;

	if (slskv_find(slsm.slsm_proctable, pid, (uintptr_t *) &slsp) != 0)
	    return;

	slskv_del(slsm.slsm_proctable, pid);
	slsp_fini(slsp);
}


/* Go through the process table of the SLS, listing all processes in it. */
static void
slsp_list(void)
{
	struct sls_process *slsp;
	struct slskv_iter iter;
	uint64_t pid;

	iter = slskv_iterstart(slsm.slsm_proctable);
	while (slskv_itercont(&iter, &pid, (uintptr_t *) &slsp) != SLSKV_ITERDONE)
	    printf("PID %ld\n", pid);
}

/* Find a process in the SLS with the given PID. */
struct sls_process *
slsp_find(pid_t pid)
{
	struct sls_process *slsp;

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
	struct sls_process *slsp;
	uint64_t pid;

	/* If we never completed initialization, abort. */
	if (slsm.slsm_proctable == NULL)
	    return;

	/* Remove all proces*/
	while (slskv_pop(slsm.slsm_proctable, &pid, (uintptr_t *) &slsp) == 0)
	    slsp_fini(slsp);
}

void
slsp_ref(struct sls_process *slsp)
{
	atomic_add_int(&slsp->slsp_refcount, 1);
}

void
slsp_deref(struct sls_process *slsp)
{
	atomic_add_int(&slsp->slsp_refcount, -1);

	if (slsp->slsp_refcount == 0)
	    slsp_del(slsp->slsp_pid);

}
