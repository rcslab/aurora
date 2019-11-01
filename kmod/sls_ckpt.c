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

#include <slos.h>
#include <sls_data.h>

#include "sls_internal.h"
#include "sls_file.h"
#include "sls_ioctl.h"
#include "sls_mm.h"
#include "sls_proc.h"
#include "sls_table.h"
#include "sls_vmspace.h"

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

static void slsckpt_stop(struct proc *p);
static void sls_checkpoint(struct proc *p, struct slspart *slsp);
static int slsckpt_metadata(struct proc *p, struct slspart *slsp);

/*
 * Stop a process, and wait until it is truly not running. 
 */
static void
slsckpt_stop(struct proc *p)
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


/*
 * Get all the metadata of a process.
 */
static int
slsckpt_metadata(struct proc *p, struct slspart *slsp)
{
	struct sbuf *sb;
	int error = 0;

	/* Dump the process state */
	PROC_LOCK(p);
	if (!SLS_PROCALIVE(p)) {
	    SLS_DBG("proc trying to die, exiting ckpt\n");
	    error = ESRCH;
	    goto out;
	}

	sb = sbuf_new_auto();

	error = slsckpt_proc(p, sb);
	if (error != 0) {
	    SLS_DBG("Error: proc_ckpt failed with error code %d\n", error);
	    goto out;
	}

	error = slsckpt_filedesc(p, sb);
	if (error != 0) {
	    SLS_DBG("Error: fd_ckpt failed with error code %d\n", error);
	    goto out;
	}

	error = slsckpt_vmspace(p, sb, slsp->slsp_attr.attr_mode);
	if (error != 0) {
	    SLS_DBG("Error: vmspace_ckpt failed with error code %d\n", error);
	    goto out;
	}

	error = sbuf_finish(sb);
	if (error != 0)
	    goto out;

	error = slskv_add(slsm.slsm_rectable, (uint64_t) p, (uintptr_t) sb);
	if (error != 0)
	    goto out;

	error = slskv_add(slsm.slsm_typetable, (uint64_t) sb, (uintptr_t) SLOSREC_PROC);
	if (error != 0)
	    goto out;

out:
	PROC_UNLOCK(p);

	if (error != 0) {
	    slskv_del(slsm.slsm_rectable, (uint64_t) sb);
	    sbuf_delete(sb);
	}

	return error;
}


/*
 * Below are the two functions used for creating the shadows, slsobj_shadow()
 * and slsobj_collapse(). What slsobj_shadow does it take a reference on the
 * objects that are directly attached to the process' entries, and then create
 * a shadow for it. The entry's reference to the original object is transferred
 * to the shadow, while the new object gets another reference to the original.
 * As a net result, if O the old object, S the shadow, and E the entry, we
 * go from 
 *
 * [O (1)] - E
 *
 * to 
 *
 * [O (2)] - [S (1)] - E
 *    |
 *   SLS
 *
 * where the numbers in parentheses are the number of references and lines 
 * denote a reference to objects. To collapse the objects, we remove the
 * reference taken by the SLS.
 */

/*
 * Get all objects accessible throught the VM entries of the checkpointed process.
 * This causes any CoW objects shared among memory maps to split into two. 
 */
static int 
slsobj_shadow(struct proc *p, struct slskv_table *objtable, struct slskv_table *newtable)
{
	struct vm_map_entry *entry, *header;
	struct slskv_table *table;
	vm_object_t obj, shadow;
	int error;

	table = (newtable != NULL) ? newtable : objtable;

	header = &p->p_vmspace->vm_map.header;
	for (entry = header->next; entry != header; entry = entry->next) {
		obj = entry->object.vm_object;
		if (obj == NULL || obj->type != OBJT_DEFAULT)
		    continue;
		
		/* Check if we have already shadowed the object. */
		if (slskv_find(table, (uint64_t) obj, (uintptr_t *) &shadow) == 0)
		    continue;

		/* 
		 * We take a reference to the old object for ourselves.
		 * That way, we avoid issues like its pages being 
		 * transferred to the shadow when the process faults.
		 */
		vm_object_reference(obj);
		vm_object_shadow(&entry->object.vm_object, &entry->offset, 
			entry->end - entry->start);

		/* Remove the mappings so that the process rereads them. */
		pmap_remove(&p->p_vmspace->vm_pmap, entry->start, entry->end);

		/* Add them to the table for dumping later. */
		error = slskv_add(table, (uint64_t) obj, (uintptr_t) entry->object.vm_object);
		if (error != 0) {
		    /* If we failed, undo the shadowing. */
		    vm_object_deallocate(obj);
		    return error;
		}

		/* 
		 * If we are doing a full checkpoint, or if this is the first
		 * of a series of delta checkpoints, we need to checkpoint all
		 * parent objects apart from the top-level ones. Otherwise we
		 * are fine with only the top-level objects. The way we can
		 * see whether we should traverse the whole object hierarchy
		 * is by checking the table argument; if there are still 
		 * objects from a previous checkpoint (the latter case) it is
		 * non-NULL.
		 */
		if (newtable == NULL)
		    continue;

		obj = obj->backing_object;
		while (obj != NULL && obj->type == OBJT_DEFAULT) {
		    /* 
		     * Take a reference to the object. We do this because
		     * it's possible to move pages from an object to one of its 
		     * shadows while we are checkpointing. If we have already
		     * dumped that shadow's pages, that means that we will miss
		     * it. By adding an extra reference we disallow this optimization.
		     */

		    vm_object_reference(obj);
		    /* These objects have no shadows. */
		    error = slskv_add(table, (uint64_t) obj, (uintptr_t) NULL);
		    if (error != 0) {
			/* If we failed, undo the shadowing. */
			vm_object_deallocate(obj);
			return error;
		    }
		}
	}

	return error;
}

/*
 * Collapse an object created by SLS into its parent.
 */
static void
slsobj_collapse(struct slskv_table *objtable)
{
	struct slskv_iter iter;
	vm_object_t obj, shadow;
	
	/* 
	 * When collapsing, we do so by removing the 
	 * reference of the original object we took in 
	 * slsobj_shadow. After having done so, the
	 * object is left with one reference and one
	 * shadow, so it can be collapsed with it.
	 */
	iter = slskv_iterstart(objtable);
	while (slskv_itercont(&iter, (uint64_t *) &obj, (uintptr_t *) &shadow) != SLSKV_ITERDONE)
	    vm_object_deallocate(obj);
}

/*
 * Checkpoint a process once. This includes stopping and restarting
 * it properly, as well as shadowing any VM objects directly accessible
 * by the partition's processes.
 */
static void 
sls_checkpoint(struct proc *p, struct slspart *slsp)
{
	struct slskv_table *newtable = NULL, *table;
	struct slos_node *vp;
	int error = 0;

	SLS_DBG("Dump created\n");

	/* This causes the process to get detached from its terminal.*/
	slsckpt_stop(p);
	SDT_PROBE0(sls, , ,stopped);
	SLS_DBG("Process stopped\n");

	/* Get all the metadata from the process. */
	error = slsckpt_metadata(p, slsp);
	if(error != 0) {
	    SLS_DBG("Checkpointing failed\n");
	    SLS_CONT(p);

	    return;
	}

	/* 
	 * Check if there are objects from a previous iteration. 
	 * If there are, we need to discern between them and those
	 * that we will create in this iteration.
	 *
	 * This check is an elegant way of checking if we are doing
	 * either a) a full checkpoint, or the first in a series of
	 * delta checkpoints, or b) a normal delta checkpoint. This
	 * matters when choosing which and how many elements to shadow. 
	 */
	error = slskv_create(&table, SLSKV_NOREPLACE);
	if (error != 0)
	    goto out;

	if (slsp->slsp_objects == NULL)
	    slsp->slsp_objects = table;
	else 
	    newtable = table;


	/* Shadow the objects to be dumped. */
	error = slsobj_shadow(p, slsp->slsp_objects, newtable);
	if (error != 0)
	    goto out;

	/* Let the process execute ASAP */
	SLS_CONT(p);
	SDT_PROBE0(sls, , ,cont);

	/* ------------ SLOS-Specific Part ------------ */

	/* XXX TEMP so that we can checkpoint multiple times. */
	slos_iremove(&slos, 1024);

	/* The dump itself. */
	/* XXX Temporary until we change to multiple inodes per checkpoint. */
	error = slos_icreate(&slos, 1024);
	if (error != 0)
	    goto out;

	vp = slos_iopen(&slos, 1024);
	if (vp == NULL)
	    goto out;

	table = (newtable != NULL) ? newtable : slsp->slsp_objects;
	error = sls_write_slos(vp, slsm.slsm_typetable, table);
	if (error != 0) {
	    SLS_DBG("sls_write_slos return %d\n", error);
	    goto out;
	}

	error = slos_iclose(&slos, vp);
	if (error != 0)
	    goto out;

	/* ------------ End SLOS-Specific Part ------------ */

	/* 
	 *  If this is a delta checkpoint, and it is not the first
	 *  one, we partially collapse the chain of shadows.
	 *  We assume a  structure where, if P the parent object 
	 *  and S the SLS-created shadow:
	 *
	 * (P) --> (S)
	 *
	 * For the next checkpoint, we shadow the shadow itself, creating
	 * the chain:
	 *
	 * (P) --> (S) --> (DS (for Double Shadow))
	 *
	 * Now we need to collapse part of the chain. Whether we collapse
	 * P with S or S with DS depends on whether we use the "deep" or
	 * "shallow" strategy, respectively, named based on how deep on
	 * the chain we collapse. If we use deep deltas, we merge the pages 
	 * of the two objects, incurring an extra cost. However, at the next
	 * iteration, the chain will be like this:
	 *
	 * (P, S) --> (DS) --> (TS)
	 *
	 * Since we get the pages to be dumped from the middle object, we
	 * have less pages to dump, because we dump only the pages touched 
	 * since the last checkpoint. If, onthe other hand, we used shallow
	 * deltas, we collapse S and DS, which logically have less pages than
	 * P for large loads (since they only hold pages touched during the
	 * last two checkpoints), but on the next checkpoint the chain will
	 * be
	 *
	 * (P) --> (S, DS) --> (TS)
	 *
	 * meaning that we dump _all_ pages touched since P was shadowed.
	 * This means that the middle object eventually holds all pages,
	 * and we should possibly do a deep checkpoint to fix that.
	 */

	if (newtable != NULL) {
	    switch (slsp->slsp_attr.attr_mode) {
	    /* XXX For now we assume deep deltas. */
	    case SLS_DELTA:
	    case SLS_DEEP:
		slsobj_collapse(slsp->slsp_objects);
		slskv_destroy(slsp->slsp_objects);
		slsp->slsp_objects = newtable;

	    case SLS_SHALLOW:
		slsobj_collapse(newtable);
		slskv_destroy(newtable);

	    default:
		panic("invalid mode %d\n", slsp->slsp_attr.attr_mode);
	    }
	}

out:

	/* 
	* If we did a full checkpoint, we don't need to keep the 
	* shadows. As a result, we remove _all_ references we
	* made, causing the shadows to collapse into the parents.
	*/
	if (slsp->slsp_attr.attr_mode == SLS_FULL)
	    slsobj_collapse(slsp->slsp_objects);

	slsp->slsp_epoch += 1;
	SLS_DBG("Checkpointed process once\n");
}

/*
 * System process that continuously checkpoints a partition.
 */
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
	    sls_checkpoint(args->p, args->slsp);
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

