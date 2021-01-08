#include <sys/types.h>
#include <sys/endian.h>
#include <sys/lock.h>
#include <sys/queue.h>

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
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/rwlock.h>
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/taskqueue.h>
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
#include <sls_data.h>

#include "sls_internal.h"
#include "sls_file.h"
#include "sls_ioctl.h"
#include "sls_proc.h"
#include "sls_sysv.h"
#include "sls_table.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"
#include "sls_vnode.h"
#include "debug.h"

#define SLS_PROCALIVE(proc) \
    (((proc)->p_state != PRS_ZOMBIE) && !((proc)->p_flag & P_WEXIT))

#define SLS_RUNNABLE(proc, slsp) \
    (atomic_load_int(&slsp->slsp_status) != 0 && \
	SLS_PROCALIVE(proc))

int sls_vfs_sync = 0;
uint64_t sls_ckpt_attempted;
uint64_t sls_ckpt_done;

SDT_PROBE_DEFINE1(sls, , sls_checkpointd, , "char *");
SDT_PROBE_DEFINE1(sls, , sls_checkpoint, , "char *");
SDT_PROBE_DEFINE1(sls, , slsckpt_metadata, , "char *");
SDT_PROBE_DEFINE0(sls, , , stopclock_start);
SDT_PROBE_DEFINE0(sls, , , stopclock_finish);

/*
 * Stop the processes, and wait until they are truly not running. 
 */
static void
slsckpt_stop(slsset *procset, struct proc *pcaller)
{
	struct slskv_iter iter;
	struct proc *p;

	KVSET_FOREACH(procset, iter, p) {
		/* The caller is already single threaded. */
		if (p == pcaller)
			continue;

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
slsckpt_cont(slsset *procset, struct proc *pcaller)
{
	struct slskv_iter iter;
	struct proc *p;

	KVSET_FOREACH(procset, iter, p) {
		/* The caller ends single threading themselves. */
		if (p == pcaller)
			continue;

		/* Free each process from the boundary. */
		PROC_LOCK(p);
		thread_single_end(p, SINGLE_BOUNDARY);
		PROC_UNLOCK(p);
	}
}

/*
 * Get all the metadata of a process.
 */
static int __attribute__ ((noinline))
slsckpt_metadata(struct proc *p, slsset *procset, struct slsckpt_data *sckpt_data)
{
	struct sls_record *rec;
	struct sbuf *sb;
	int error = 0;

	KASSERT(p->p_pid != 0, ("Trying to checkpoint the kernel"));

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

	SDT_PROBE1(sls, , slsckpt_metadata, , "Setting up the metadata");

	error = slsckpt_vmspace(p->p_vmspace, sb, sckpt_data);
	if (error != 0) {
		SLS_DBG("Error: slsckpt_vmspace failed with error code %d\n", error);
		goto out;
	}

	SDT_PROBE1(sls, , slsckpt_metadata, , "Checkpointing vm state");

	error = slsckpt_proc(p, sb, procset, sckpt_data);
	if (error != 0) {
		SLS_DBG("Error: slsckpt_proc failed with error code %d\n", error);
		goto out;
	}

	SDT_PROBE1(sls, , slsckpt_metadata, , "Checkpointing process state");
	/* XXX This has to be last right now, because the filedesc is not in its 
	 * own record. */
	error = slsckpt_filedesc(p, sckpt_data, sb);
	if (error != 0) {
		SLS_DBG("Error: slsckpt_filedesc failed with error code %d\n", error);
		goto out;
	}

	SDT_PROBE1(sls, , slsckpt_metadata, , "Checkpointing file table");

	error = sbuf_finish(sb);
	if (error != 0)
		goto out;

	rec = sls_getrecord(sb, (uint64_t) p, SLOSREC_PROC);
	error = slskv_add(sckpt_data->sckpt_rectable, (uint64_t) p, (uintptr_t) rec);
	if (error != 0) {
		free(rec, M_SLSREC);
		goto out;
	}

	SDT_PROBE1(sls, , slsckpt_metadata, , "Creating record");

out:

	if (error != 0) {
		slskv_del(sckpt_data->sckpt_rectable, (uint64_t) sb);
		sbuf_delete(sb);
	}

	SDT_PROBE1(sls, , slsckpt_metadata, , "Creating record");

	return error;
}

static bool
slsckpt_isfullckpt(struct slspart *slsp)
{

	if (slsp->slsp_target == SLS_MEM)
		return (false);

	return (slsp->slsp_target == SLS_FULL);
}

static void
slsckpt_compact_single(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	struct sls_record *oldrec, *rec;
	uint64_t slsid;
	int error;

	KASSERT(slsp->slsp_target == SLS_MEM || slsp->slsp_mode == SLS_DELTA,
	    ("Invalid single object compaction"));

	/* Replace any metadata records we have in the old table. */
	KV_FOREACH_POP(sckpt_data->sckpt_rectable, slsid, rec) {
		slskv_pop(slsp->slsp_sckpt->sckpt_rectable, &slsid, (uintptr_t *) &oldrec);
		free(oldrec, M_SLSREC);
		error = slskv_add(slsp->slsp_sckpt->sckpt_rectable, slsid, 
		    (uintptr_t) rec);
		KASSERT(error == 0, ("Could not add new record"));
	}

	/* Collapse the new table into the old one. */
	slsvm_objtable_collapsenew(slsp->slsp_sckpt->sckpt_objtable, 
	    sckpt_data->sckpt_objtable);
	slsckpt_destroy(sckpt_data, NULL);
}

static void
slsckpt_compact(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	if (slsp->slsp_target == SLS_MEM || slsp->slsp_mode == SLS_DELTA) {
		/* Replace the old checkpoint in the partition. */
		slsckpt_destroy(slsp->slsp_sckpt, sckpt_data);
		slsp->slsp_sckpt = sckpt_data;
		return;
	}

	/* Compact the ful checkpoint. */
	KASSERT(slsp->slsp_mode == SLS_FULL, ("invalid mode %d\n", slsp->slsp_mode));
		/* Destroy the shadows. We don't keep any between iterations. */
	DEBUG("Compacting full checkpoint");
	KASSERT(slsp->slsp_sckpt == NULL, ("Full disk checkpoint has data"));
	slsckpt_destroy(sckpt_data, NULL);
}

static int
slsckpt_initio(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	int error;

	KASSERT(slsp->slsp_target == SLS_OSD, ("invalid target for IO"));

	/* Create a record for every vnode. */
	/*
	 * Actually serializing vnodes proves costly for applications with 
	 * hundreds of vnodes. We avoid it completely for in-memory checkponts, 
	 * and defer it to after the partition has resumed if using disk.
	 */
	error = slsckpt_vnode_serialize(sckpt_data);
	if (error != 0)
		return (error);

	error = sls_write_slos(slsp->slsp_oid, sckpt_data);
	if (error != 0) {
		DEBUG1("Writing to disk failed with %d", error);
		return (error);
	}

	return (0);
}

/*
 * Checkpoint a process once. This includes stopping and restarting
 * it properly, as well as shadowing any VM objects directly accessible
 * by the partition's processes.
 */
static int __attribute__ ((noinline))
sls_checkpoint(slsset *procset, struct proc *pcaller, struct slspart *slsp)
{
	struct slsckpt_data *sckpt_data;
	struct slskv_iter iter;
	struct thread *td;
	struct proc *p;
	int error = 0;

	DEBUG("Process stopped");
#ifdef KTR
	KVSET_FOREACH(procset, iter, p) {
		slsvm_print_vmspace(p->p_vmspace);
	}
#endif
	error = slsckpt_create(&sckpt_data, &slsp->slsp_attr);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , sls_checkpoint, , "Creating the checkpoint struct");

	/* Shadow SYSV shared memory. */
	error = slsckpt_sysvshm(sckpt_data, sckpt_data->sckpt_objtable);
	if (error != 0) {
		DEBUG1("Checkpointing SYSV failed with %d\n", error);
		slsckpt_cont(procset, pcaller);
		goto error;
	}

	SDT_PROBE1(sls, , sls_checkpoint, , "Getting System V shared memory");

	/* Get the data from all processes in the partition. */
	KVSET_FOREACH(procset, iter, p) {
		error = slsckpt_metadata(p, procset, sckpt_data);
		if(error != 0) {
			DEBUG2("Checkpointing process %d failed with %d\n",
			    p->p_pid, error);
			KV_ABORT(iter);
			slsckpt_cont(procset, pcaller);
			goto error;
		}
	}

	SDT_PROBE1(sls, , sls_checkpoint, , "Getting the metadata");

	KVSET_FOREACH(procset, iter, p) {
		KASSERT(P_SHOULDSTOP(p), ("process not stopped"));
		FOREACH_THREAD_IN_PROC(p, td)
			KASSERT(TD_IS_INHIBITED(td), ("thread is not inhibited"));
	}

	/* Shadow the objects to be dumped. */
	error = slsvm_procset_shadow(procset, sckpt_data->sckpt_objtable,
	    slsckpt_isfullckpt(slsp));
	if (error != 0) {
		DEBUG1("shadowing failed with %d\n", error);
		slsckpt_cont(procset, pcaller);
		goto error;
	}

	SDT_PROBE1(sls, , sls_checkpoint, , "Shadowing the objects");

	/* Let the process execute ASAP */
	slsckpt_cont(procset, pcaller);

	SDT_PROBE1(sls, , sls_checkpoint, , "Continuing the process");
	SDT_PROBE0(sls, , , stopclock_finish);

	/* Initiate IO, if necessary. */
	if (slsp->slsp_target == SLS_OSD) {
		error = slsckpt_initio(slsp, sckpt_data);
		if (error != 0)
			DEBUG1("slsckpt_initio failed with %d", error);
	}

	SDT_PROBE1(sls, , sls_checkpoint, , "Initiating IO");

	/* 
	 * Collapse the shadows. Do it before making the partition available to 
	 * safely execute with region checkpoints.
	 */
	slsckpt_compact(slsp, sckpt_data);

	SDT_PROBE1(sls, , sls_checkpoint, , "Compacting the address space");

	/* 
	 * Mark the partition as available. That way, region checkpoints 
	 * progress while we wait for the taskqueue to be drained. There is 
	 * possible interference in that draining the taskqueue might include 
	 * draining IOs initiated by other checkpoints, but that shouldn't be 
	 * too much of a problem.
	 */
	error = slsp_setstate(slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(error == 0, ("partition not in ckpt state"));

	/* Drain the taskqueue, ensuring all IOs have hit the disk. */
	if (slsp->slsp_target == SLS_OSD) {
		taskqueue_drain_all(slos.slos_tq);
		/* XXX Using MNT_WAIT is causing a deadlock right now. */
		VFS_SYNC(slos.slsfs_mount, (sls_vfs_sync != 0) ? MNT_WAIT : MNT_NOWAIT);
	}

	SDT_PROBE1(sls, , sls_checkpoint, , "Draining taskqueue");

	/* Advance the current major epoch. */
	slsp_epoch_advance_major(slsp);

	DEBUG("Checkpointed partition once");

	return (0);

error:
	/* Undo existing VM object tree modifications. */
	if (sckpt_data != NULL)
		slsckpt_destroy(sckpt_data, NULL);

	return (error);
}

/*
 * Get the entry and object associated with the region.
 */
static int
slsckpt_dataregion_getvm(struct slspart *slsp, struct proc *p,
	vm_ooffset_t addr, struct slsckpt_data *sckpt_data,
	vm_map_entry_t *entryp, vm_object_t *objp)
{
	vm_map_entry_t entry;
	boolean_t contained;
	vm_object_t obj;
	vm_map_t map;

	map = &p->p_vmspace->vm_map;

	/* Can't work with submaps. */
	if (map->flags & MAP_IS_SUB_MAP)
		return (EINVAL);

	/* Find the entry holding the object. .*/
	vm_map_lock(map);
	contained = vm_map_lookup_entry(map, addr, &entry);
	vm_map_unlock(map);
	if (!contained)
		return (EINVAL);

	/* Requests must be aligned to a map entry. */
	if (entry->start != addr)
		return (EINVAL);

	/* Cannot work with submaps or wired entries. */
	if (entry->eflags & (MAP_ENTRY_IS_SUB_MAP | MAP_ENTRY_USER_WIRED))
		return (EINVAL);

	obj = entry->object.vm_object;
	if (!OBJT_ISANONYMOUS(obj))
		return (EINVAL);

	*entryp = entry;
	*objp = obj;

	return (0);
}

/*
 * Fill the checkpoint data structure with the region's data and metadata.
 */
static int
slsckpt_dataregion_fillckpt(struct slspart *slsp, struct proc *p,
    vm_ooffset_t addr, struct slsckpt_data *sckpt_data)
{
	vm_map_entry_t entry;
	vm_object_t obj;
	int error;

	/* Get the VM entities that hold the relevant information. */
	error = slsckpt_dataregion_getvm(slsp, p, addr, sckpt_data, &entry, &obj);
	if (error != 0)
		return (error);

	KASSERT(OBJT_ISANONYMOUS(obj), ("getting metadata for non anonymous obj"));

	/* Only objects referenced only by the entry can be shadowed. */
	if (obj->ref_count > 1)
		return (EINVAL);

	/*Get the metadata of the VM object. */
	error = slsckpt_vmobject(obj, sckpt_data);
	if (error != 0)
		return (error);

	/* Get the data and shadow it for the entry. */
	error = slsvm_entry_shadow(p, sckpt_data->sckpt_objtable, entry,
	    slsckpt_isfullckpt(slsp));
	if (error != 0)
		return (error);


	return (0);
}

static bool
slsckpt_proc_in_part(struct slspart *slsp, struct proc *p)
{
	struct slskv_iter iter;
	uint64_t pid;

	/* Make sure the process is in the partition. */
	KVSET_FOREACH(slsp->slsp_procs, iter, pid) {
		if (p->p_pid == (pid_t) pid) {
			KV_ABORT(iter);
			return (true);
		}
	}

	return (false);
}

/*
 * Persist an area of memory of the process. The process must be in a partition.
 * At restore time, the data region has the data grabbed here, instead of the 
 * ones created during the previous checkpoint.
 */
int
slsckpt_dataregion(struct slspart *slsp, struct proc *p, vm_ooffset_t addr)
{
	struct slsckpt_data *sckpt_data = NULL;
	int stateerr, error;

	/*
	 * Unless doing full checkpoints (which are there just for benchmarking 
	 * purposes), we have to have checkpointed the whole partition before 
	 * checkpointing individual regions.
	 */
	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_target == SLS_DELTA))
		if (slsp->slsp_sckpt == NULL)
			return (EINVAL);

	/* Wait till we can start checkpointing. */
	if (slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_CHECKPOINTING, true) != 0) {

		/* Once a partition is detached its state cannot change.  */
		KASSERT(slsp->slsp_status == SLSP_DETACHED,
			("Blocking slsp_setstate() on live partition failed"));

		/* Tried to checkpoint a removed partition. */
		return (EINVAL);
	}

	/*
	 * Once a process is in a partition it is there forever, so there can be 
	 * no races with the call below.
	 */
	if (!slsckpt_proc_in_part(slsp, p)) {
		error  = EINVAL;
		goto error_single;
	}

	error = slsckpt_create(&sckpt_data, &slsp->slsp_attr);
	if (error != 0)
		goto error_single;

	/* Single thread to avoid races with other threads. */
	PROC_LOCK(p);
	thread_single(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	/* Add the data and metadata. This also shadows the object. */
	error =  slsckpt_dataregion_fillckpt(slsp, p, addr, sckpt_data);
	if (error != 0) {
		error = EINVAL;
		goto error;
	}

	/* The process can continue while we are dumping. */
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	if (slsp->slsp_target == SLS_OSD) {
		error = sls_write_slos_dataregion(sckpt_data);
		if (error != 0)
			DEBUG1("slsckpt_initio failed with %d", error);
	}

	/*
	 * We compact before turning the checkpoint available, because we modify 
	 * the partition's current checkpoint data.
	 */
	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_target == SLS_DELTA))
		slsckpt_compact_single(slsp, sckpt_data);
	else
		slsckpt_destroy(sckpt_data, NULL);

	stateerr = slsp_setstate(slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("partition not in ckpt state"));

	/* Drain the taskqueue, ensuring all IOs have hit the disk. */
	if (slsp->slsp_target == SLS_OSD) {
		taskqueue_drain_all(slos.slos_tq);
		/* XXX Using MNT_WAIT is causing a deadlock right now. */
		VFS_SYNC(slos.slsfs_mount, (sls_vfs_sync != 0) ? MNT_WAIT : MNT_NOWAIT);
	}

	/* Only advance the minor epoch, major epochs are full checkpoints. */
	slsp_epoch_advance_minor(slsp);

	return (0);

error:
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

error_single:

	stateerr = slsp_setstate(slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("partition not in ckpt state"));

	slsckpt_destroy(sckpt_data, NULL);

	return (error);
}

static int
slsckpt_gather_processes(struct slspart *slsp, struct proc *pcaller, slsset *procset)
{
	struct slskv_iter iter;
	uint64_t deadpid = 0;
	struct proc *p;
	uint64_t pid;
	int error;

	KVSET_FOREACH(slsp->slsp_procs, iter, pid) {

		/* Remove the PID of the unavailable process from the set. */
		if (deadpid != 0) {
			slsp_detach(slsp->slsp_oid, deadpid);
			deadpid = 0;
		}

		/* Does the process still exist? */
		error = pget(pid, PGET_WANTREAD, &p);
		if (error != 0) {
			deadpid = pid;
			continue;
		}

		/* Is the process exiting? */
		if (!SLS_RUNNABLE(p, slsp)) {
			PRELE(p);
			deadpid = pid;
			continue;
		}

		/* Add it to the set of processes to be checkpointed. */
		error = slsset_add_unlocked(procset, (uint64_t) p);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}
	}

	/* Cleanup any leftover dead PIDs. */
	if (deadpid != 0)
		slsp_detach(slsp->slsp_oid, deadpid);

	return (0);
}

/* Gather the children of the processes currently in the working set. */
static int
slsckpt_gather_children_once(slsset *procset, struct proc *pcaller, int *new_procs)
{
	struct proc *p, *pchild;
	struct slskv_iter iter;
	int error;

	/* Stop all processes in the set. */
	slsckpt_stop(procset, pcaller);

	/* Assume there are no new children. */
	new_procs = 0;

	KVSET_FOREACH(procset, iter, p) {
		/* Check for new children. */
		LIST_FOREACH(pchild, &p->p_children, p_sibling) {
			if (slsset_find_unlocked(procset, (uint64_t) pchild) != 0) {
				/* We found a child that didn't exist before. */
				if (!SLS_PROCALIVE(pchild))
					continue;

				new_procs += 1;

				PHOLD(pchild);
				error = slsset_add_unlocked(procset, (uint64_t) pchild);
				if (error != 0) {
					KV_ABORT(iter);
					return (error);
				}

			}
		}
	}

	return (0);
}

/* Gather all descendants of the processes in the working set. */
static int
slsckpt_gather_children(slsset *procset, struct proc *pcaller)
{
	int new_procs;
	int error;

	do {
		error = slsckpt_gather_children_once(procset, pcaller, &new_procs);
		if (error != 0)
			return (error);
	} while (new_procs > 0);

	return (0);
}

/*
 * System process that continuously checkpoints a partition.
 */
void __attribute__ ((noinline))
sls_checkpointd(struct sls_checkpointd_args *args)
{
	struct proc *pcaller = args->pcaller;
	struct slspart *slsp = args->slsp;
	struct timespec tstart, tend;
	long msec_elapsed, msec_left;
	slsset *procset = NULL;
	struct proc *p;
	int stateerr, error = 0;

	/* The set of processes we are going to checkpoint. */
	error = slsset_create(&procset);
	if (error != 0) {
		sls_ckpt_attempted += 1;
		goto out;
	}

	for (;;) {
		nanotime(&tstart);

		sls_ckpt_attempted += 1;

		/*
		 * Check if the partition is available for checkpointing. If the 
		 * operation fails, the partition is detached. Note that this 
		 * allows multiple checkpoint daemons on the same partition.
		 */
		if (slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_CHECKPOINTING, true) != 0) {

			/* Once a partition is detached its state cannot change.  */
			KASSERT(slsp->slsp_status == SLSP_DETACHED,
			    ("Blocking slsp_setstate() on live partition failed"));

			goto out;
		}

		/* See if we're destroying the module. */
		if (slsm.slsm_exiting != 0)
			break;

		sls_ckpt_attempted += 1;
		DEBUG1("Attempting checkpoint %d", sls_ckpt_attempted);
		/* Check if the partition got detached from the SLS. */
		if (slsp->slsp_status != SLSP_CHECKPOINTING)
			break;

		SDT_PROBE1(sls, , sls_checkpointd, , "Setting up");

		/* Gather all processes still running. */
		error = slsckpt_gather_processes(slsp, pcaller, procset);
		if (error != 0)
			break;

		if (slsp_isempty(slsp))
			break;

		/*
		 * If we recursively checkpoint, we don't actually enter the children
		 * into the SLS permanently, but only checkpoint them in this iteration.
		 * This only matters if the parent dies, in which case the children will
		 * not be checkpointed anymore; this makes sense because we mainly want
		 * the children because they might be part of the state of the parent,
		 * if we actually care about them we can add them to the SLS.
		 */

		/* If we're not recursively checkpointing, abort the search. */
		if (args->recurse != 0) {
			error = slsckpt_gather_children(procset, pcaller);
			if (error != 0)
				break;
		}

		SDT_PROBE1(sls, , sls_checkpointd, , "Gathering processes");
		SDT_PROBE0(sls, , , stopclock_start);

		slsckpt_stop(procset, pcaller);

		SDT_PROBE1(sls, , sls_checkpointd, , "Stopping processes");

		/* Checkpoint the process once. */
		error = sls_checkpoint(procset, pcaller, slsp);
		if (error != 0) {
			DEBUG1("Checkpoint failed with %d\n", error);
			break;
		}

		nanotime(&tend);

		sls_ckpt_done += 1;

		/* Release all checkpointed processes. */
		KVSET_FOREACH_POP(procset, p)
		    PRELE(p);

		/* If the interval is 0, checkpointing is non-periodic. Finish up. */
		if (slsp->slsp_attr.attr_period == 0)
			goto out;

		/* Else compute how long we need to wait until we need to checkpoint again. */
		msec_elapsed = (TONANO(tend) - TONANO(tstart)) / (1000 * 1000);
		msec_left = slsp->slsp_attr.attr_period - msec_elapsed;
		if (msec_left > 0)
			pause_sbt("slscpt", SBT_1MS * msec_left, 0, C_HARDCLOCK | C_CATCH);

		DEBUG("Woke up");
		/* 
		 * The sls_checkpoint() process has marked the partition as 
		 * available after it was done initiating dumping to disk. 
		 */
	}

	/*
	 * If we exited normally, and the process is still in the SLOS,
	 * mark the process as available for checkpointing.
	 */
	stateerr = slsp_setstate(slsp, SLSP_CHECKPOINTING,
	    SLSP_AVAILABLE, false);

	KASSERT(stateerr == 0, ("partition in state %d", slsp->slsp_status));

	DEBUG("Stopped checkpointing");

out:

	/*
	 * Either everything went ok, or something went wrong before or during
	 * checkpointing proper. In any case, propagate the error if the caller
	 * is waiting for it.
	 */
	if (slsp->slsp_attr.attr_period == 0)
		slsp_signal(slsp, error);

	/* Drop the reference we got for the SLS process. */
	slsp_deref(slsp);

	if (procset != NULL) {
		/* Release any process references gained. */
		KVSET_FOREACH_POP(procset, p)
		    PRELE(p);
		slskv_destroy(procset);
	}

	/* Free the arguments of the kthread, and the module reference. */
	free(args, M_SLSMM);
	sls_finishop();

	kthread_exit();
}

