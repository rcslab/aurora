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
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <sys/pipe.h>

#include <vm/pmap.h>
#include <vm/vm.h>
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

#include "sls_internal.h"
#include "sls_file.h"
#include "sls_ioctl.h"
#include "sls_mm.h"
#include "sls_proc.h"
#include "sls_sysv.h"
#include "sls_table.h"
#include "sls_vmspace.h"

#define SLS_PROCALIVE(proc) \
    (((proc)->p_state != PRS_ZOMBIE) && !((proc)->p_flag & P_WEXIT))

#define SLS_RUNNABLE(proc, slsp) \
    (atomic_load_int(&slsp->slsp_status) != 0 && \
    SLS_PROCALIVE(proc))

#define OBJT_ISANONYMOUS(obj) \
	(((obj) != NULL) && \
	(((obj)->type == OBJT_DEFAULT) || \
	 ((obj)->type == OBJT_SWAP)))

SDT_PROVIDER_DEFINE(sls);

SDT_PROBE_DEFINE(sls, , , start);
SDT_PROBE_DEFINE(sls, , , stopped);
SDT_PROBE_DEFINE(sls, , , sysv);
SDT_PROBE_DEFINE(sls, , , proc);
SDT_PROBE_DEFINE(sls, , , file);
SDT_PROBE_DEFINE(sls, , , shadow);
SDT_PROBE_DEFINE(sls, , , pmap_start);
SDT_PROBE_DEFINE(sls, , , pmap_done);
SDT_PROBE_DEFINE(sls, , , vm);
SDT_PROBE_DEFINE(sls, , , cont);
SDT_PROBE_DEFINE(sls, , , dump);
SDT_PROBE_DEFINE(sls, , , done);

static void slsckpt_stop(slsset *procset);
static int sls_checkpoint(slsset *procset, struct slspart *slsp);
static int slsckpt_metadata(struct proc *p, struct slspart *slsp, 
	slsset *procset, struct slsckpt_data *sckpt_data);

static inline uint64_t
tonano(struct timespec tp)
{
    const long billion = 1000UL * 1000 * 1000;

    return billion * tp.tv_sec + tp.tv_nsec;
}

/*
 * Stop the processes, and wait until they are truly not running. 
 */
static void
slsckpt_stop(slsset *procset)
{
	struct slskv_iter iter;
	struct proc *p;
	
	KVSET_FOREACH(procset, iter, p) {
	    /* Force all threads to the kernel-user boundary. */ 
	    PROC_LOCK(p);
	    thread_single(p, SINGLE_BOUNDARY);
	    PROC_UNLOCK(p);
	}
}

/* 
 * Let all processes continue executing.  
 */
static void
slsckpt_cont(slsset *procset)
{
	struct slskv_iter iter;
	struct proc *p;

	KVSET_FOREACH(procset, iter, p) {
	    /* Free each process from the boundary. */
	    PROC_LOCK(p);
	    thread_single_end(p, SINGLE_BOUNDARY);
	    PROC_UNLOCK(p);
	}
}

/*
 * Get all the metadata of a process.
 */
static int
slsckpt_metadata(struct proc *p, struct slspart *slsp, slsset *procset, struct slsckpt_data *sckpt_data)
{
	struct sls_record *rec;
	struct sbuf *sb;
	int error = 0;

	/* Dump the process state */
	PROC_LOCK(p);
	if (!SLS_PROCALIVE(p)) {
	    SLS_DBG("proc trying to die, exiting ckpt\n");
	    error = ESRCH;
	    PROC_UNLOCK(p);
	    goto out;
	}
	PROC_UNLOCK(p);

	sb = sbuf_new_auto();

	error = slsckpt_proc(p, sb, procset);
	if (error != 0) {
	    SLS_DBG("Error: slsckpt_proc failed with error code %d\n", error);
	    goto out;
	}
	SDT_PROBE0(sls, , , proc);

	error = slsckpt_vmspace(p, sb, sckpt_data, slsp->slsp_attr.attr_target);
	if (error != 0) {
	    SLS_DBG("Error: slsckpt_vmspace failed with error code %d\n", error);
	    goto out;
	}

	SDT_PROBE0(sls, , , vm);
	/* XXX This has to be last right now, because the filedesc is not in its own record. */
	error = slsckpt_filedesc(p, sckpt_data, sb);
	if (error != 0) {
	    SLS_DBG("Error: slsckpt_filedesc failed with error code %d\n", error);
	    goto out;
	}

	SDT_PROBE0(sls, , , file);
	error = sbuf_finish(sb);
	if (error != 0)
	    goto out;

	rec = sls_getrecord(sb, SLOSREC_PROC);
	error = slskv_add(sckpt_data->sckpt_rectable, (uint64_t) p, (uintptr_t) rec);
	if (error != 0) {
	    free(rec, M_SLSMM);
	    goto out;
	}

out:

	if (error != 0) {
	    slskv_del(sckpt_data->sckpt_rectable, (uint64_t) sb);
	    sbuf_delete(sb);
	}

	return error;
}

/* Transfer a reference between objects. */
static void
slsvm_object_reftransfer(vm_object_t src, vm_object_t dst)
{
	vm_object_reference(dst);
	vm_object_deallocate(src);
}

/* Create a shadow of the same size as the object, perfectly aligned. */
static void
slsvm_object_shadowexact(vm_object_t *objp)
{
	vm_ooffset_t offset = 0;

	vm_object_shadow(objp, &offset, ptoa((*objp)->size));
}

/* 
 * Shadow an object. Take an extra reference on behalf of Aurora for the
 * object, and transfer the one already in the object to the shadow. The
 * transfer follows the following logic: The object has a reference because
 * some memory location in some remote data structure holds a pointer to it.  
 * Pass that exact location to the vm_object_shadow() function to replace the
 * pointer with one to the new object.
 *
 * Save the object-shadow pair in the table.
 */
static int
slsvm_object_shadow(struct slskv_table *objtable, vm_object_t *objp)
{
	vm_object_t obj;
	int error;

	obj = *objp;
	vm_object_reference(obj);
	slsvm_object_shadowexact(objp);

	error = slskv_add(objtable, (uint64_t) obj, (uintptr_t) *objp);
	if (error != 0) {
	    vm_object_deallocate(*objp);
	    return (error);
	}

	return (0);
}

void
slsvm_objtable_collapse(struct slskv_table *objtable)
{
	struct slskv_iter iter;
	vm_object_t obj, shadow;
	
	/* Remove the Aurora - created shadow. */
	KV_FOREACH(objtable, iter, obj, shadow) 
	    vm_object_deallocate(obj);

	/* XXX Don't forget the SYSV shared memory table! */
}

/* Destroy all physical process memory mappings for the entry. */
static void
slsvm_entry_zap(struct proc *p, struct vm_map_entry *entry)
{
	SDT_PROBE0(sls, , , pmap_start);
	pmap_remove(&p->p_vmspace->vm_pmap, entry->start, entry->end);
	SDT_PROBE0(sls, , , pmap_done);
}

/*
 * Below are the two functions used for creating the shadows, slsvm_procset_shadow()
 * and slsvm_objtable_collapse(). What slsvm_procset_shadow does it take a reference on the
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
slsvm_proc_shadow(struct proc *p, struct slskv_table *table, int is_fullckpt)
{
	struct vm_map_entry *entry, *header;
	vm_object_t obj, vmshadow;
	int error;

	header = &p->p_vmspace->vm_map.header;
	for (entry = header->next; entry != header; entry = entry->next) {
		obj = entry->object.vm_object;
		/* 
		 * Non anonymous objects cannot shadowed meaningfully. Guard 
		 * entries are null.
		 */
		if (!OBJT_ISANONYMOUS(obj))
		    continue;

		/* 
		 * Check if we have already shadowed the object. If we did, 
		 * have the process map point to the shadow.
		 */
		if (slskv_find(table, (uint64_t) obj, (uintptr_t *) &vmshadow) == 0) {
		    entry->object.vm_object = vmshadow;
		    slsvm_object_reftransfer(obj, vmshadow);
		    slsvm_entry_zap(p, entry);
		    continue;
		}

		/* Shadow the object, retain it in Aurora. */
		error = slsvm_object_shadow(table, &entry->object.vm_object);
		if (error != 0)
		    goto error;

		slsvm_entry_zap(p, entry);

		if (!is_fullckpt)
		    continue;

		/* If in a full checkpoint, checkpoint down the tree. */

		obj = obj->backing_object;
		while (OBJT_ISANONYMOUS(obj)) {
		    vm_object_reference(obj);
		    /* These objects have no shadows. */
		    error = slskv_add(table, (uint64_t) obj, (uintptr_t) NULL);
		    if (error != 0) {
			/* Already in the table. */
			vm_object_deallocate(obj);
			break;
		    }

		    obj = obj->backing_object;
		}
	}

	return (0);

error:

	/* Deallocate only the shadows we have control over. */
	slskv_destroy(table);

	return (error);
}

/* Collapse the backing objects of all processes under checkpoint. */
static int 
slsvm_procset_shadow(slsset *procset, struct slskv_table *table, int is_fullckpt) 
{
	struct slskv_iter iter;
	struct proc *p;
	int error;

	KVSET_FOREACH(procset, iter, p) {
	    error = slsvm_proc_shadow(p, table, is_fullckpt);
	    if (error != 0)
		return (error);
	}

	return (0);
}

/*
 * Checkpoint a process once. This includes stopping and restarting
 * it properly, as well as shadowing any VM objects directly accessible
 * by the partition's processes.
 */
static int 
sls_checkpoint(slsset *procset, struct slspart *slsp)
{
	struct slsckpt_data *sckpt_data, *sckpt_old = NULL;
	struct slskv_iter iter;
	struct slos_node *vp;
	int is_fullckpt;
	struct proc *p;
	int error = 0;

	SLS_DBG("Process stopped\n");

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
	error = slsckpt_create(&sckpt_data);
	if (error != 0)
	    return (error);

	/* 
	 * If there is no previous checkpoint, then we are doing a full checkpoint. 
	 * This is regardless of whether we have SLS_FULL as a mode; we might be 
	 * doing deltas, with this being the first iteration.
	 */
	is_fullckpt = (slsp->slsp_sckpt == NULL) ? 1 : 0;
	if (!is_fullckpt)
	    sckpt_old = slsp->slsp_sckpt;

	/* Shadow SYSV shared memory. */
	error = slsckpt_sysvshm(sckpt_data, sckpt_data->sckpt_objtable);
	if (error != 0) {
	    slsckpt_cont(procset);
	    goto error;
	}

	SDT_PROBE0(sls, , , sysv);

	/* Get the data from all processes in the partition. */
	KVSET_FOREACH(procset, iter, p) {
	    error = slsckpt_metadata(p, slsp, procset, sckpt_data);
	    if(error != 0) {
		SLS_DBG("Checkpointing failed\n");
		slsckpt_cont(procset);
		goto error;
	    }
	}

	/* Shadow the objects to be dumped. */
	error = slsvm_procset_shadow(procset, sckpt_data->sckpt_objtable, is_fullckpt);
	if (error != 0) {
	    SLS_DBG("shadowing failed with %d\n", error);
	    slsckpt_cont(procset);
	    goto error;
	}

	SDT_PROBE0(sls, , , shadow);
	/* Let the process execute ASAP */
	slsckpt_cont(procset);
	SDT_PROBE0(sls, , ,cont);

	switch (slsp->slsp_attr.attr_target) {
	case SLS_OSD:
	    /* 
	    * The cleanest way for this to go away is by splitting
	    * different SLS records to different on-disk inodes. This
	    * is definitely possible, and depending on the SLOS it can
	    * even be faster. However, right now it's not very useful,
	    * since the SLOS isn't there yet in terms of speed.
	    */
	    /* XXX TEMP so that we can checkpoint multiple times. */
	    slos_iremove(&slos, slsp->slsp_oid);

	    /* The dump itself. */
	    /* XXX Temporary until we change to multiple inodes per checkpoint. */
	    error = slos_icreate(&slos, slsp->slsp_oid, 0);
	    if (error != 0)
		goto error;

	    vp = slos_iopen(&slos, slsp->slsp_oid);
	    if (vp == NULL)
		goto error;

	    error = sls_write_slos(vp, sckpt_data);
	    if (error != 0) {
		SLS_DBG("sls_write_slos return %d\n", error);
		goto error;
	    }

	    error = slos_iclose(&slos, vp);
	    if (error != 0)
		goto error;

	    break;

	    /*
	     * XXX Sync all the data to the SLOS, when we have a way to do so.
	     */

	case SLS_MEM:
	    /* Associate the checkpoint with the partition. */
	    slsp->slsp_sckpt = sckpt_data;
	    sckpt_data = NULL;
	    break;

	default:
	    panic("Invalid target %d\n", slsp->slsp_attr.attr_target);
	}
	SDT_PROBE0(sls, , , dump);


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

	/* 
	 * XXX In-memory checkpoints are ONLY full checkpoints (the very concept
	 * of a delta checkpoint is not applicable if you think about it). This
	 * in turn means that we are free to assume we can freely destroy any
	 * checkpoints we have in hand below, because they are not reachable 
	 * by the partition.
	 */
	switch (slsp->slsp_attr.attr_mode) {
	case SLS_FULL:
	    /* Destroy the shadows completely. We don't keep any between iterations. */
	    slsckpt_destroy(sckpt_data);
	    sckpt_data = NULL;
	    break;

	/* XXX For now we assume deep deltas. */
	case SLS_DELTA:
	case SLS_DEEP:
	    /* Destroy the old shadows, now only the new ones remain. */
	    if (sckpt_old != NULL)
		slsckpt_destroy(sckpt_old);
	    slsp->slsp_sckpt = sckpt_data;
	    break;

	case SLS_SHALLOW:
	    /* Destory the new shadow, provided we already have an old one. */
	    if (sckpt_old != NULL)
		slsckpt_destroy(sckpt_data);
	    else 
		slsp->slsp_sckpt = sckpt_data;
	    break;

	default:
	    panic("invalid mode %d\n", slsp->slsp_attr.attr_mode);
	}

	slsp->slsp_epoch += 1;
	SLS_DBG("Checkpointed partition once\n");
	SDT_PROBE0(sls, , , done);

	return (0);

error:
	/* Undo existing VM object tree modifications. */
	if (sckpt_data != NULL)
	    slsckpt_destroy(sckpt_data);

	return (error);
}

/*
 * System process that continuously checkpoints a partition.
 */
void
sls_checkpointd(struct sls_checkpointd_args *args)
{
	struct timespec tstart, tend;
	long msec_elapsed, msec_left;
	uint64_t pid, deadpid = 0;
	struct slskv_iter iter;
	slsset *procset = NULL;
	struct proc *p, *pchild;
	int new_procs;
	int error;

	SLS_DBG("Process active\n");
	/* 
	 * Check if the partition is available for checkpointing. 
	 * If not, silently exit - the parition is already being
	 * checkpointed due to a previous call. 
	 */
	if (atomic_cmpset_int(&args->slsp->slsp_status, SPROC_AVAILABLE, 
		    SPROC_CHECKPOINTING) == 0) {
	    SLS_DBG("Partition %ld in state %d\n", args->slsp->slsp_oid, args->slsp->slsp_status);
	    goto out;
	}

	/* The set of processes we are going to checkpoint. */
	error = slskv_create(&procset);
	if (error != 0)
	    goto out;

	for (;;) {
	    /* 
	     * If the partition has changed state, we have to stop
	     * checkpointing because it got detached from the SLS.
	     */
	    if (args->slsp->slsp_status != SPROC_CHECKPOINTING)
		break;

	    /* 
	     * If deadpid is valid (!= 0), we need to remove it from the process set,
	     * because the process it corresponds to is unavailable. We need to delay
	     * the freeing until after we iterate through the slsset entry, due to
	     * the latter's implementation.
	     */
	    deadpid = 0;

	    KVSET_FOREACH(args->slsp->slsp_procs, iter, pid) {

		/* Remove the PID of the unavailable process from the set. */
		if (deadpid != 0) {
		    slsset_del(args->slsp->slsp_procs, deadpid);
		    slsset_del(slsm.slsm_procs, deadpid);

		    deadpid = 0;
		}

		/* 
		 * Check if the process we are trying to checkpoint is trying
		 * to exit, if so not only drop the reference the daemon has, 
		 * but also that of the SLS itself.
		 */
		error = pget(pid, PGET_WANTREAD, &p);
		if (error != 0 || !SLS_RUNNABLE(p, args->slsp)) {
		    SLS_DBG("Getting a process %ld failed\n", pid);
		    /* Drop the reference taken by pget. */
		    PRELE(p);
		    deadpid = pid;
		    continue;
		}


		/* Add it to the set of processes to be checkpointed. */
		error = slsset_add(procset, (uint64_t) p);
		if (error != 0)
		    goto out;

	    }

	    nanotime(&tstart);
	    /* 
	     * If we recursively checkpoint, we don't actually enter the children
	     * into the SLS permanently, but only checkpoint them in this iteration.
	     * This only matters if the parent dies, in which case the children will
	     * not be checkpointed anymore; this makes sense because we mainly want
	     * the children because they might be part of the state of the parent,
	     * if we actually care about them we can add them to the SLS.
	     */

	    do { 
		/* If we're not recursively checkpointing, abort the search. */
		if (args->recurse == 0)
		    break;

		/* Assume there are no new children. */
		new_procs = 0;

		/* 
		 * Stop all processes in the set. This causes them
		 * to get detached from their terminals.
		 */
		slsckpt_stop(procset);
		KVSET_FOREACH(procset, iter, p) {
		    /* 
		     * Check every process if it had a 
		     * new child while we were checking.
		     */ 
		    LIST_FOREACH(pchild, &p->p_children, p_sibling) {
			if (slskv_find(procset, (uint64_t) pchild, (uintptr_t *) &pchild) != 0) {
			    /* We found a child that didn't exist before. */

			    /* 
			     * Check if the child is still alive.
			     * If it's exiting ignore its subtree.
			     */
			    if (!SLS_PROCALIVE(pchild))
				continue;

			    new_procs += 1;

			    PHOLD(pchild);
			    /* 
			     * Here we're adding to the set while we're iterating it.
			     * While whether we will iterate through the new element
			     * is undefined, we will go through the set again in the
			     * next iteration of the outer while, so it's safe.
			     */
			    error = slsset_add(procset, (uint64_t) pchild);
			    if (error != 0)
				goto out;

			}
		    }
		}
	    } while (new_procs > 0);

	    SDT_PROBE0(sls, , , start);
	    slsckpt_stop(procset);
	    SDT_PROBE0(sls, , , stopped);

	    /* Checkpoint the process once. */
	    sls_checkpoint(procset, args->slsp);
	    nanotime(&tend);

	    /* Release all checkpointed processes. */
	    SET_FOREACH_POP(procset, p)
		PRELE(p);

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

	/* Cleanup any dead PIDs still around. */
	if (deadpid != 0) {
	    slsset_del(args->slsp->slsp_procs, deadpid);
	    slsset_del(slsm.slsm_procs, deadpid);
	    deadpid = 0;
	}

	printf("Checkpointing for partition %ld done.\n", args->slsp->slsp_oid);

	/* Drop the reference we got for the SLS process. */
	slsp_deref(args->slsp);

	if (procset != NULL) {
	    /* Release any process references gained. */
	    SET_FOREACH_POP(procset, p)
		PRELE(p);
	    slskv_destroy(procset);
	}

	/* Free the arguments passed to the kthread. */
	free(args, M_SLSMM);

	kthread_exit();
}

