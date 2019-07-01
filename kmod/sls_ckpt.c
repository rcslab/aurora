#include <sys/types.h>

#include <sys/capsicum.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "sls.h"
#include "sls_channel.h"
#include "sls_data.h"
#include "sls_fd.h"
#include "sls_ioctl.h"
#include "sls_mem.h"
#include "sls_proc.h"
#include "slsmm.h"

#include "sls_dump.h"
#include "../include/slos.h"
#include "../slos/slos_internal.h"
#include "../slos/slos_inode.h"

#define SLS_SIG(p, sig) \
	PROC_LOCK(p); \
	kern_psignal(p, sig); \
	PROC_UNLOCK(p); \

#define SLS_CONT(p) SLS_SIG(p, SIGCONT)
    
#define SLS_STOP(p) SLS_SIG(p, SIGSTOP)

#define SLS_PROCALIVE(proc) \
    (((proc)->p_state != PRS_ZOMBIE) && !((proc)->p_flag & P_WEXIT))

#define SLS_RUNNABLE(proc, slsp) \
    (atomic_load_int(&slsp->slsp_status) != 0 && \
    SLS_PROCALIVE(proc))

SDT_PROVIDER_DEFINE(sls);

SDT_PROBE_DEFINE(sls, , , stopped);
SDT_PROBE_DEFINE(sls, , , cont);

void sls_stop_proc(struct proc *p);
void sls_ckpt_once(struct proc *p, struct sls_process *slsp);
int sls_ckpt(struct proc *p, struct sls_process *slsp);

void
sls_stop_proc(struct proc *p)
{
	int threads_still_running;
	struct thread *td;
	
	SLS_STOP(p);

	threads_still_running = 1;
	while (threads_still_running == 1) {
	    threads_still_running = 0;
	    PROC_LOCK(p);
	    TAILQ_FOREACH(td, &p->p_threads, td_plist) {
		if (TD_IS_RUNNING(td)) {
		    threads_still_running = 1;
		    break;
		}
	    }
	    if(!SLS_PROCALIVE(p)) {
		SLS_DBG("Proc trying to die, exiting stop\n");
		PROC_UNLOCK(p);
		break;
	    }
	    PROC_UNLOCK(p);
	    pause_sbt("slsrun", 50 * SBT_1US, 0 , C_DIRECT_EXEC | C_CATCH);
	}
}


int
sls_ckpt(struct proc *p, struct sls_process *slsp)
{
	int error = 0;

	/* Dump the process state */
	PROC_LOCK(p);
	if (!SLS_PROCALIVE(p)) {
	    SLS_DBG("proc trying to die, exiting ckpt\n");
	    error = ESRCH;
	    goto sls_ckpt_out;
	}

	error = sls_proc_ckpt(p, slsp->slsp_ckptbuf);
	if (error != 0) {
	    SLS_DBG("Error: proc_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}

	error = sls_filedesc_ckpt(p, slsp->slsp_ckptbuf);
	if (error != 0) {
	    SLS_DBG("Error: fd_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}

	error = sls_vmspace_ckpt(p, slsp->slsp_ckptbuf, slsp->slsp_attr.attr_mode);
	if (error != 0) {
	    SLS_DBG("Error: vmspace_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}


sls_ckpt_out:
	PROC_UNLOCK(p);

	return error;
}

void 
sls_ckpt_once(struct proc *p, struct sls_process *slsp)
{
    vm_ooffset_t new_charge;
    struct sls_channel chan;
    struct vmspace *new_vm;
    int error = 0;

    SLS_DBG("Dump created\n");

    /* This causes the process to get detached from its terminal.*/
    sls_stop_proc(p);
    SDT_PROBE0(sls, , ,stopped);
    SLS_DBG("Process stopped\n");

    error = sls_ckpt(p, slsp);
    if(error != 0) {
	SLS_DBG("Checkpointing failed\n");
	SLS_CONT(p);

	return;
    }


    /* XXX Create new option for deep deltas */
    /*
    old_vm = slsp->slsp_vm;
    old_charge = slsp->slsp_charge;
    */

    new_vm = vmspace_fork(p->p_vmspace, &new_charge);

    /* XXX Code only valid for deep deltas, uncomment when ready */
    /*
    if (slsp->slsp_vm == NULL) {
	printf("Error: Shadowing vmspace failed\n");
	slss_fini(slss);
	return;
    }
    */

    if (swap_reserve_by_cred(new_charge, proc0.p_ucred) == 0) {
	printf("Error: Could not reserve swap space\n");
	vmspace_free(slsp->slsp_vm);
	return;
    }
    SLS_DBG("New vmspace created\n");

    if (slsp->slsp_epoch == 0) {
	slsp->slsp_vm = new_vm;
	slsp->slsp_charge = new_charge;
    }

    /* Let the process execute ASAP */
    SLS_CONT(p);
    SDT_PROBE0(sls, , ,cont);

    if (slsp->slsp_epoch > 0 && slsp->slsp_attr.attr_mode == SLS_FULL) {
	vmspace_free(new_vm);
	SLS_DBG("Full compaction complete\n");
    }

    /* Create the channel that will be used to send the data to the backend. */
    error = slschan_init(&slsp->slsp_attr.attr_backend, &chan);
    if (error != 0)
	return;

    /* The dump itself. */
    error = sls_dump(slsp, &chan);

    /* Close up the channel before going checking the error value. */
    slschan_fini(&chan);

    /* Check whether we dumped successfully. */
    if (error != 0)
	return;

    if (slsp->slsp_epoch > 0 && slsp->slsp_attr.attr_mode == SLS_DELTA) {
	vmspace_free(new_vm);
	SLS_DBG("Delta compaction complete\n");
    }

    slsp->slsp_epoch += 1;
    SLS_DBG("Checkpointed process once\n");
}

void
sls_checkpointd(struct sls_checkpointd_args *args)
{
	struct timespec tstart, tend;
	long msec_elapsed, msec_left;

	SLS_DBG("Process active\n");
	/* 
	 * Check if the process is available for checkpointing. 
	 * If not, silently exit - the process is already
	 * being checkpointed due to a previous call. 
	 */
	if (atomic_cmpset_int(&args->slsp->slsp_status, SPROC_AVAILABLE, 
		    SPROC_CHECKPOINTING) == 0) {
	    SLS_DBG("Process %d in state %d\n", args->p->p_pid, args->slsp->slsp_status);
	    goto out;

	}

	for (;;) {
	    /* 
	     * If the process has changed state, we have to stop
	     * checkpointing because it got detached from the SLS.
	     */
	    if (args->slsp->slsp_status != SPROC_CHECKPOINTING)
		break;

	    /* 
	     * Check if the process we are trying to checkpoint is trying
	     * to exit, if so not only drop the reference the daemon has, 
	     * but also that of the SLS itself.
	     */
	    if (!SLS_RUNNABLE(args->p, args->slsp)) {
		SLS_DBG("Process %d no longer runnable\n", args->p->p_pid);
		slsp_deref(args->slsp);
		break;
	    }

	    /* Checkpoint the process once. */
	    nanotime(&tstart);
	    sls_ckpt_once(args->p, args->slsp);
	    nanotime(&tend);

	    args->slsp->slsp_epoch += 1;


	    /* If the interval is 0, checkpointing is non-periodic. Finish up. */
	    if (args->slsp->slsp_attr.attr_period == 0)
		break;

	    /* Else compute how long we need to wait until we need to checkpoint again. */
	    msec_elapsed = (tonano(tend) - tonano(tstart)) / (1000 * 1000);
	    msec_left = args->slsp->slsp_attr.attr_period - msec_elapsed;
	    if (msec_left > 0)
		pause_sbt("slscpt", SBT_1MS * msec_left, 0, C_HARDCLOCK | C_CATCH);

	    SLS_DBG("Woke up\n");
	}

	/* 
	 * 
	 * If we exited normally, and the process is still in the SLOS,
	 * mark the process as available for checkpointing.
	 */
	atomic_cmpset_int(&args->slsp->slsp_status, SPROC_CHECKPOINTING, 
		    SPROC_AVAILABLE); 

	SLS_DBG("Stopped checkpointing\n");

out:

	printf("Checkpointing for process %d done.\n", args->p->p_pid);

	/* Drop the reference we got for the SLS process. */
	slsp_deref(args->slsp);

	/* Also release the reference for the FreeBSD process. */
	PRELE(args->p);


	/* Free the arguments passed to the kthread. */
	free(args, M_SLSMM);

	kthread_exit();
}

