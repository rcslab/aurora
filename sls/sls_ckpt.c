#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/selinfo.h>
#include <sys/capsicum.h>
#include <sys/conf.h>
#include <sys/endian.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/pcpu.h>
#include <sys/pipe.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/taskqueue.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <slos.h>
#include <slos_inode.h>
#include <sls_data.h>

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"
#include "sls_ioctl.h"
#include "sls_proc.h"
#include "sls_sysv.h"
#include "sls_table.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"
#include "sls_vnode.h"

#define SLS_PROCALIVE(proc) \
	(((proc)->p_state != PRS_ZOMBIE) && !((proc)->p_flag & P_WEXIT))

#define SLS_RUNNABLE(proc, slsp) \
	(atomic_load_int(&slsp->slsp_status) != 0 && SLS_PROCALIVE(proc))

int sls_vfs_sync = 0;
int sls_only_flush_deltas = 0;
uint64_t sls_ckpt_attempted;
uint64_t sls_ckpt_done;
uint64_t sls_ckpt_duration;

SDT_PROBE_DEFINE1(sls, , slsckpt_dataregion_dump, , "char *");
SDT_PROBE_DEFINE1(sls, , slsckpt_dataregion_fillckpt, , "char *");
SDT_PROBE_DEFINE1(sls, , slsckpt_dataregion, , "char *");
SDT_PROBE_DEFINE1(sls, , sls_checkpointd, , "char *");
SDT_PROBE_DEFINE1(sls, , sls_ckpt, , "char *");
SDT_PROBE_DEFINE0(sls, , , stopclock_start);
SDT_PROBE_DEFINE0(sls, , , meta_start);
SDT_PROBE_DEFINE0(sls, , , stopclock_finish);
SDT_PROBE_DEFINE0(sls, , , meta_finish);

/*
 * Stop the processes, and wait until they are truly not running.
 */
void
slsckpt_stop(slsset *procset, struct proc *pcaller)
{
	struct slskv_iter iter;
	struct proc *p;

	KVSET_FOREACH(procset, iter, p)
	{
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
void
slsckpt_cont(slsset *procset, struct proc *pcaller)
{
	struct slskv_iter iter;
	struct proc *p;

	KVSET_FOREACH(procset, iter, p)
	{
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
static int
slsckpt_metadata(
    struct proc *p, slsset *procset, struct slsckpt_data *sckpt_data)
{
	struct sbuf *sb;
	int error = 0;

	SDT_PROBE1(sls, , sls_ckpt, , "Starting metadata checkpointing");
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

	SDT_PROBE1(sls, , sls_ckpt, , "Setting up metadata");

	error = slsvmspace_checkpoint(p->p_vmspace, sb, sckpt_data);
	if (error != 0) {
		SLS_DBG("Error: slsckpt_vmspace failed with error code %d\n",
		    error);
		goto out;
	}

	SDT_PROBE1(sls, , sls_ckpt, , "Checkpointing vm state");

	error = slsproc_checkpoint(p, sb, procset, sckpt_data);
	if (error != 0) {
		SLS_DBG(
		    "Error: slsckpt_proc failed with error code %d\n", error);
		goto out;
	}

	SDT_PROBE1(sls, , sls_ckpt, , "Getting process state");
	/* XXX This has to be last right now, because the filedesc is not in its
	 * own record. */
	error = slsckpt_filedesc(p, sckpt_data, sb);
	if (error != 0) {
		SLS_DBG("Error: slsckpt_filedesc failed with error code %d\n",
		    error);
		SDT_PROBE1(sls, , sls_ckpt, , "Getting the fdtable");
		goto out;
	}

	SDT_PROBE1(sls, , sls_ckpt, , "Getting the fdtable");

	error = sbuf_finish(sb);
	if (error != 0)
		goto out;

	error = slsckpt_addrecord(sckpt_data, (uint64_t)p, sb, SLOSREC_PROC);

out:

	SDT_PROBE1(sls, , sls_ckpt, , "Metadata done");
	return (error);
}

static bool
slsckpt_isfullckpt(struct slspart *slsp)
{

	if (slsp->slsp_target == SLS_MEM)
		return (false);

	return (slsp->slsp_target == SLS_FULL);
}

static void
slsckpt_compact_single(struct slspart *slsp, struct slsckpt_data *sckpt)
{
	struct sls_record *oldrec, *rec;
	uint64_t slsid;
	int error;

	KASSERT(slsp->slsp_target == SLS_MEM || slsp->slsp_mode == SLS_DELTA,
	    ("Invalid single object compaction"));

	/* Replace any metadata records we have in the old table. */
	KV_FOREACH_POP(sckpt->sckpt_rectable, slsid, rec)
	{
		slskv_pop(slsp->slsp_sckpt->sckpt_rectable, &slsid,
		    (uintptr_t *)&oldrec);
		sls_record_destroy(oldrec);
		error = slskv_add(
		    slsp->slsp_sckpt->sckpt_rectable, slsid, (uintptr_t)rec);
		KASSERT(error == 0, ("Could not add new record"));
	}

	/* Collapse the new table into the old one. */
	slsvm_objtable_collapsenew(
	    slsp->slsp_sckpt->sckpt_shadowtable, sckpt->sckpt_shadowtable);
	slsckpt_drop(sckpt);
}

void
slsckpt_compact(struct slspart *slsp, struct slsckpt_data *sckpt)
{
	struct slsckpt_data *old_sckpt;
	struct slskv_table *objtable;

	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_mode == SLS_DELTA)) {

		/* Replace the old checkpoint in the partition. */
		old_sckpt = slsp->slsp_sckpt;
		slsp->slsp_sckpt = sckpt;

		if (old_sckpt != NULL) {
			objtable = (sckpt != NULL) ? sckpt->sckpt_shadowtable :
						     0;
			slsvm_objtable_collapse(
			    old_sckpt->sckpt_shadowtable, objtable);
			slsckpt_drop(old_sckpt);
		}

		return;
	}

	/* Compact the ful checkpoint. */
	KASSERT(slsp->slsp_mode == SLS_FULL,
	    ("invalid mode %d\n", slsp->slsp_mode));
	/* Destroy the shadows. We don't keep any between iterations. */
	DEBUG("Compacting full checkpoint");
	KASSERT(slsp->slsp_sckpt == NULL, ("Full disk checkpoint has data"));
	slsckpt_drop(sckpt);
}

static int
slsckpt_io_slos(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	int error;

	error = sls_write_slos(slsp->slsp_oid, sckpt_data);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , sls_ckpt, , "Initiating IO to disk");

	/* Drain the taskqueue, ensuring all IOs have hit the disk. */
	taskqueue_drain_all(slos.slos_tq);
	error = slsfs_wakeup_syncer(0);

	SDT_PROBE1(sls, , sls_ckpt, , "Draining taskqueue");

	return (error);
}

static int
slsckpt_io_file(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	struct vnode *vp = (struct vnode *)slsp->slsp_backend;
	struct thread *td = curthread;
	int error;

	error = sls_write_file(slsp, sckpt_data);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , sls_ckpt, , "Initiating IO to disk");

	VOP_LOCK(vp, LK_EXCLUSIVE);
	error = VOP_FSYNC(vp, MNT_WAIT, td);
	VOP_UNLOCK(vp, 0);

	SDT_PROBE1(sls, , sls_ckpt, , "Draining taskqueue");

	return (error);
}

static int
slsckpt_io_socket(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	int error;

	error = sls_write_socket(slsp, sckpt_data);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , sls_ckpt, , "Initiating IO to remote server");

	/* The write routine is currently synchronous, no draining necessary. */

	return (error);
}

static int
slsckpt_initio(struct slspart *slsp, struct slsckpt_data *sckpt_data)
{
	int error;

	/* No need to flush memory backends. */
	if (slsp->slsp_target == SLS_MEM)
		return (0);

	/* Create a record for every vnode. */
	/*
	 * Actually serializing vnodes proves costly for applications with
	 * hundreds of vnodes. We avoid it completely for in-memory checkponts,
	 * and defer it to after the partition has resumed if using disk.
	 */
	error = slsckpt_vnode_serialize(sckpt_data);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , sls_ckpt, , "Serializing vnodes");

	error = sbuf_finish(sckpt_data->sckpt_meta);
	if (error != 0)
		return (error);

	error = sbuf_finish(sckpt_data->sckpt_dataid);
	if (error != 0)
		return (error);

	/* Initiate IO, if necessary. */
	switch (slsp->slsp_target) {
	case SLS_SOCKSND:
		return (EOPNOTSUPP);

	case SLS_SOCKRCV:
		return (EOPNOTSUPP);

	case SLS_FILE:
		return (EOPNOTSUPP);

	case SLS_OSD:
		return (slsckpt_io_slos(slsp, sckpt_data));

	case SLS_MEM:
		return (0);

	default:
		panic("using invalid backend %d\n", slsp->slsp_target);
	}
}

/*
 * Checkpoint a process once. This includes stopping and restarting
 * it properly, as well as shadowing any VM objects directly accessible
 * by the partition's processes.
 */
static int __attribute__((noinline)) sls_ckpt(slsset *procset,
    struct proc *pcaller, struct slspart *slsp, uint64_t nextepoch)
{
	struct slsckpt_data *sckpt;
	struct slskv_iter iter;
	struct thread *td;
	struct proc *p;
	int error = 0;

	/* Zero out the Metropolis process ID. It will be set if needed. */
	slsp->slsp_metr.slsmetr_proc = 0;

	DEBUG("Process stopped");
#ifdef KTR
	KVSET_FOREACH(procset, iter, p) { slsvm_print_vmspace(p->p_vmspace); }
#endif
	sckpt = slsckpt_alloc(&slsp->slsp_attr);
	if (sckpt == NULL)
		return (ENOMEM);

	SDT_PROBE0(sls, , , meta_start);
	SDT_PROBE1(sls, , sls_ckpt, , "Creating the checkpoint");

	/* Shadow SYSV shared memory. */
	error = slsckpt_sysvshm(sckpt);
	if (error != 0) {
		DEBUG1("Checkpointing SYSV failed with %d\n", error);
		slsckpt_cont(procset, pcaller);
		goto error;
	}

	SDT_PROBE1(sls, , sls_ckpt, , "Getting System V memory");

	/* Get the data from all processes in the partition. */
	KVSET_FOREACH(procset, iter, p)
	{

		/* Insert the process into Aurora. */
		SLS_LOCK();
		PROC_LOCK(p);
		/* Transient processes are not in Metropolis mode. */
		sls_procadd(slsp->slsp_oid, p, false);
		PROC_UNLOCK(p);
		SLS_UNLOCK();

		error = slsckpt_metadata(p, procset, sckpt);
		if (error != 0) {
			DEBUG2("Checkpointing process %d failed with %d\n",
			    p->p_pid, error);
			KV_ABORT(iter);
			slsckpt_cont(procset, pcaller);
			goto error;
		}
	}

	SDT_PROBE1(sls, , sls_ckpt, , "Getting the metadata");

	SDT_PROBE0(sls, , , meta_finish);
	/* Shadow the objects to be dumped. */
	error = slsvm_procset_shadow(procset, sckpt, slsckpt_isfullckpt(slsp));
	if (error != 0) {
		DEBUG1("shadowing failed with %d\n", error);
		slsckpt_cont(procset, pcaller);
		goto error;
	}

	SDT_PROBE1(sls, , sls_ckpt, , "Shadowing the objects");

	KVSET_FOREACH(procset, iter, p)
	{
		KASSERT(P_SHOULDSTOP(p), ("process not stopped"));
		FOREACH_THREAD_IN_PROC (p, td)
			KASSERT(
			    TD_IS_INHIBITED(td), ("thread is not inhibited"));
	}

	/*
	 * Let the process execute ASAP. For a one-off checkpoint, the process
	 * is also waiting for the partition to signal the operation is done.
	 */
	slsckpt_cont(procset, pcaller);

	if (slsp->slsp_attr.attr_period == 0)
		slsp_signal(slsp, 0);

	SDT_PROBE1(sls, , sls_ckpt, , "Continuing the process");
	SDT_PROBE0(sls, , , stopclock_finish);

	/*
	 * HACK: For Metropolis we want to measure
	 * the storage density only of deltas, since
	 * full checkpoint space usage is amortized.
	 * Use a sysctl to blackhole full checkpoints.
	 */
	if (!sls_only_flush_deltas ||
	    ((slsp->slsp_mode == SLS_DELTA) && (slsp->slsp_sckpt != NULL))) {
		error = slsckpt_initio(slsp, sckpt);
		if (error != 0)
			DEBUG1("slsckpt_initio failed with %d", error);
	}

	/* Advance the current major epoch. */
	slsp_epoch_advance(slsp, nextepoch);

	/*
	 * Collapse the shadows. Do it before making the partition available to
	 * safely execute with region checkpoints.
	 */
	slsckpt_compact(slsp, sckpt);

	/*
	 * Mark the partition as available. We don't have pipelining, so we wait
	 * for the taskqueue to drain before making the partition available
	 * again.
	 */
	error = slsp_setstate(slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(error == 0, ("partition not in ckpt state"));

	DEBUG("Checkpointed partition once");

	return (0);

error:
	/* Undo existing VM object tree modifications. */
	if (sckpt != NULL)
		slsckpt_drop(sckpt);

	if (slsp->slsp_attr.attr_period == 0)
		slsp_signal(slsp, error);

	return (error);
}

/*
 * Get the entry and object associated with the region.
 */
static int
slsckpt_dataregion_getvm(struct slspart *slsp, struct proc *p,
    vm_ooffset_t addr, struct slsckpt_data *sckpt_data, vm_map_entry_t *entryp,
    vm_object_t *objp)
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
	error = slsckpt_dataregion_getvm(
	    slsp, p, addr, sckpt_data, &entry, &obj);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , slsckpt_dataregion_fillckpt, , "Getting the object");
	KASSERT(
	    OBJT_ISANONYMOUS(obj), ("getting metadata for non anonymous obj"));

	/* Only objects referenced only by the entry can be shadowed. */
	if (obj->ref_count > 1)
		return (EINVAL);

	/*Get the metadata of the VM object. */
	error = slsvmobj_checkpoint(obj, sckpt_data);
	if (error != 0)
		return (error);

	SDT_PROBE1(
	    sls, , slsckpt_dataregion_fillckpt, , "Object checkpointing");
	/* Get the data and shadow it for the entry. */
	error = slsvm_entry_shadow(p, sckpt_data->sckpt_shadowtable, entry,
	    slsckpt_isfullckpt(slsp), true);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , slsckpt_dataregion_fillckpt, , "Object shadowing");

	return (0);
}

static bool
slsckpt_proc_in_part(struct slspart *slsp, struct proc *p)
{
	struct slskv_iter iter;
	uint64_t pid;

	/* Make sure the process is in the partition. */
	KVSET_FOREACH(slsp->slsp_procs, iter, pid)
	{
		if (p->p_pid == (pid_t)pid) {
			KV_ABORT(iter);
			return (true);
		}
	}

	return (false);
}

struct slsckpt_dataregion_dump_args {
	struct slsckpt_data *sckpt_data;
	struct slspart *slsp;
	uint64_t nextepoch;
};

static void
slsckpt_dataregion_dump(struct slsckpt_dataregion_dump_args *args)
{
	struct slsckpt_data *sckpt_data = args->sckpt_data;
	uint64_t nextepoch = args->nextepoch;
	struct slspart *slsp = args->slsp;
	int stateerr, error;

	if (slsp->slsp_target == SLS_OSD) {
		error = sls_write_slos_dataregion(sckpt_data);
		if (error != 0)
			DEBUG1("slsckpt_initio failed with %d", error);
	}

	SDT_PROBE1(sls, , slsckpt_dataregion_dump, , "Writing out the data");

	/*
	 * We compact before turning the checkpoint available, because we modify
	 * the partition's current checkpoint data.
	 */
	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_mode == SLS_DELTA))
		slsckpt_compact_single(slsp, sckpt_data);
	else
		slsckpt_drop(sckpt_data);

	SDT_PROBE1(sls, , slsckpt_dataregion_dump, , "Compacting the data");

	/* Drain the taskqueue, ensuring all IOs have hit the disk. */
	if (slsp->slsp_target == SLS_OSD) {
		taskqueue_drain_all(slos.slos_tq);
		/* XXX Using MNT_WAIT is causing a deadlock right now. */
		VFS_SYNC(slos.slsfs_mount,
		    (sls_vfs_sync != 0) ? MNT_WAIT : MNT_NOWAIT);
	}

	SDT_PROBE1(sls, , slsckpt_dataregion_dump, , "Draining the taskqueue");

	slsp_epoch_advance(slsp, nextepoch);

	SDT_PROBE1(sls, , slsckpt_dataregion_dump, , "Advancing the epoch");

	stateerr = slsp_setstate(
	    slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("partition not in ckpt state"));

	slsp_deref(slsp);

	free(args, M_SLSMM);

	/* Remove the reference taken by the initial ioctl call. */
	kthread_exit();
}

/*
 * Persist an area of memory of the process. The process must be in a partition.
 * At restore time, the data region has the data grabbed here, instead of the
 * ones created during the previous checkpoint.
 */
int
slsckpt_dataregion(struct slspart *slsp, struct proc *p, vm_ooffset_t addr,
    uint64_t *nextepoch)
{
	struct slsckpt_dataregion_dump_args *dump_args;
	struct slsckpt_data *sckpt = NULL;
	int stateerr, error;

	/*
	 * Unless doing full checkpoints (which are there just for benchmarking
	 * purposes), we have to have checkpointed the whole partition before
	 * checkpointing individual regions.
	 */
	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_mode == SLS_DELTA))
		if (slsp->slsp_sckpt == NULL)
			return (EINVAL);

	/* Wait till we can start checkpointing. */
	if (slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_CHECKPOINTING, true) !=
	    0) {

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
		error = EINVAL;
		goto error_single;
	}

	sckpt = slsckpt_alloc(&slsp->slsp_attr);
	if (sckpt == NULL) {
		error = ENOMEM;
		goto error_single;
	}

	SDT_PROBE0(sls, , , stopclock_start);
	SDT_PROBE0(sls, , , meta_start);

	SDT_PROBE1(sls, , slsckpt_dataregion, , "Getting the partition");

	/* Single thread to avoid races with other threads. */
	PROC_LOCK(p);
	thread_single(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	SDT_PROBE1(sls, , slsckpt_dataregion, , "Single threading");

	/* Add the data and metadata. This also shadows the object. */
	error = slsckpt_dataregion_fillckpt(slsp, p, addr, sckpt);
	if (error != 0) {
		error = EINVAL;
		goto error;
	}

	SDT_PROBE1(sls, , slsckpt_dataregion, , "Filling in the data");

	/* Preadvance the epoch, the kthread will advance it. */
	*nextepoch = slsp_epoch_preadvance(slsp);

	/* The process can continue while we are dumping. */
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	SDT_PROBE0(sls, , , stopclock_finish);

	/*
	 * Create a thread to asynchronously take care of the dumping.
	 * The caller can wait for the epoch to advance using sls_epoch_wait().
	 */
	dump_args = malloc(sizeof(*dump_args), M_SLSMM, M_WAITOK);
	*dump_args = (struct slsckpt_dataregion_dump_args) {
		.slsp = slsp,
		.sckpt_data = sckpt,
		.nextepoch = *nextepoch,
	};
	error = kthread_add((void (*)(void *))slsckpt_dataregion_dump,
	    dump_args, NULL, NULL, 0, 0, "slsckpt_dataregion_dump");
	if (error != 0) {
		DEBUG1("ERROR: sls_dataregion_dump failed with %d", error);
		slsp_epoch_advance(slsp, *nextepoch);
		return (error);
	}

	return (0);

error:
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	SDT_PROBE0(sls, , , stopclock_finish);

error_single:

	stateerr = slsp_setstate(
	    slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("partition not in ckpt state"));

	slsckpt_drop(sckpt);

	/* Remove the reference taken by the initial ioctl call. */
	slsp_deref(slsp);

	return (error);
}

static int
slsckpt_gather_processes(
    struct slspart *slsp, struct proc *pcaller, slsset *procset)
{
	struct slskv_iter iter;
	uint64_t deadpid = 0;
	struct proc *p;
	uint64_t pid;
	int error;

	KVSET_FOREACH(slsp->slsp_procs, iter, pid)
	{

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
		error = slsset_add(procset, (uint64_t)p);
		if (error != 0) {
			PRELE(p);
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
slsckpt_gather_children_once(
    slsset *procset, struct proc *pcaller, int *new_procs)
{
	struct proc *p, *pchild;
	struct slskv_iter iter;
	int error;

	/* Stop all processes in the set. */
	slsckpt_stop(procset, pcaller);

	/* Assume there are no new children. */
	new_procs = 0;

	KVSET_FOREACH(procset, iter, p)
	{
		/* Check for new children. */
		LIST_FOREACH (pchild, &p->p_children, p_sibling) {
			if (slsset_find(procset, (uint64_t)pchild) != 0) {
				/* We found a child that didn't exist before. */
				if (!SLS_PROCALIVE(pchild))
					continue;

				new_procs += 1;

				PHOLD(pchild);
				error = slsset_add(procset, (uint64_t)pchild);
				if (error != 0) {
					KV_ABORT(iter);
					PRELE(pchild);
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
		error = slsckpt_gather_children_once(
		    procset, pcaller, &new_procs);
		if (error != 0)
			return (error);
	} while (new_procs > 0);

	return (0);
}

int
slsckpt_gather(
    struct slspart *slsp, slsset *procset, struct proc *pcaller, bool recurse)
{
	int error;

	/* Gather all processes still running. */
	error = slsckpt_gather_processes(slsp, pcaller, procset);
	if (error != 0) {
		slsp_signal(slsp, error);
		DEBUG1("Failed to gather processes with error %d", error);
		return (error);
	}

	if (slsp_isempty(slsp)) {
		DEBUG("No processes left to checkpoint");
		slsp_signal(slsp, 0);
		return (EINVAL);
	}

	/*
	 * If we recursively checkpoint, we don't actually enter the
	 * children into the SLS permanently, but only checkpoint them
	 * in this iteration. This only matters if the parent dies, in
	 * which case the children will not be checkpointed anymore;
	 * this makes sense because we mainly want the children because
	 * they might be part of the state of the parent, if we actually
	 * care about them we can add them to the SLS.
	 */

	/* If we're not checkpointing recursively we're done. */
	if (!recurse)
		return (0);

	error = slsckpt_gather_children(procset, pcaller);
	if (error != 0) {
		slsp_signal(slsp, error);
		return (error);
	}

	return (0);
}

bool
slsckpt_prepare_state(struct slspart *slsp, bool *retry)
{
	/*
	 * Check if the partition is available for checkpointing. If the
	 * operation fails, the partition is detached. Note that this
	 * allows multiple checkpoint daemons on the same partition.
	 */
	if (slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_CHECKPOINTING, true) !=
	    0) {

		/* A detached partition cannot change state. */
		KASSERT(slsp->slsp_status == SLSP_DETACHED,
		    ("Blocking slsp_setstate() on live partition failed"));

		if (retry != NULL)
			*retry = true;
		return (false);
	}

	/* See if we're destroying the module. */
	if (slsm.slsm_exiting != 0) {
		if (retry != NULL)
			*retry = false;
		return (false);
	}

	/* Check if the partition got detached from the SLS. */
	if (slsp->slsp_status != SLSP_CHECKPOINTING) {
		DEBUG("Process not in checkpointing state, exiting");
		if (retry != NULL)
			*retry = false;
		return (false);
	}

	return (true);
}

/*
 * System process that continuously checkpoints a partition.
 */
void
sls_checkpointd(struct sls_checkpointd_args *args)
{
	struct proc *pcaller = args->pcaller;
	struct slspart *slsp = args->slsp;
	struct timespec tstart, tend;
	long msec_elapsed, msec_left;
	bool recurse = args->recurse;
	int stateerr, error = 0;
	slsset *procset = NULL;
	uint64_t localepoch;
	uint64_t *nextepoch;
	struct proc *p;
	bool retry;
	const long period = slsp->slsp_attr.attr_period;

	/* Use local storage for the epoch if the caller doesn't care. */
	nextepoch = args->nextepoch;
	if (nextepoch == NULL)
		nextepoch = &localepoch;

	/* Free the arguments, everything is now in the local stack. */
	free(args, M_SLSMM);

	/* The set of processes we are going to checkpoint. */
	error = slsset_create(&procset);
	if (error != 0) {
		sls_ckpt_attempted += 1;
		goto out;
	}

	for (;;) {
		nanotime(&tstart);

		sls_ckpt_attempted += 1;

		if (!slsckpt_prepare_state(slsp, &retry)) {
			if (period == 0 || retry == false) {
				break;
			}

			pause_sbt("slscpt", SBT_1MS * period, 0,
			    C_HARDCLOCK | C_CATCH);
			continue;
		}

		DEBUG1("Attempting checkpoint %d", sls_ckpt_attempted);
		error = slsckpt_gather(slsp, procset, pcaller, recurse);
		if (error != 0) {
			DEBUG1("slsckpt_prepare returned %d\n", error);
			break;
		}

		DEBUG("Gathered all processes");

		slsckpt_stop(procset, pcaller);

		DEBUG("Stopped all processes");

		/* Preadvance the epoch, the checkpoint will advance it. */
		*nextepoch = slsp_epoch_preadvance(slsp);

		/* Checkpoint the process once. */
		error = sls_ckpt(procset, pcaller, slsp, *nextepoch);
		if (error != 0) {
			slsp_epoch_advance(slsp, *nextepoch);
			slsp_signal(slsp, error);
			DEBUG1("Checkpoint failed with %d\n", error);
			break;
		}

		nanotime(&tend);

		sls_ckpt_done += 1;

		/* Release all checkpointed processes. */
		KVSET_FOREACH_POP(procset, p)
		PRELE(p);

		/* If the interval is 0, checkpointing is non-periodic. Finish
		 * up. */
		if (period == 0)
			goto out;

		/* Else compute how long we need to wait until we need to
		 * checkpoint again. */
		msec_elapsed = (TONANO(tend) - TONANO(tstart)) / (1000 * 1000);
		msec_left = period - msec_elapsed;
		if (msec_left > 0)
			pause_sbt("slscpt", SBT_1MS * msec_left, 0,
			    C_HARDCLOCK | C_CATCH);

		DEBUG("Woke up");
		/*
		 * The sls_ckpt() process has marked the partition as
		 * available after it was done initiating dumping to disk.
		 */
	}

	/*
	 * If we exited normally, and the process is still in the SLOS,
	 * mark the process as available for checkpointing.
	 */
	stateerr = slsp_setstate(
	    slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE, false);

	KASSERT(stateerr == 0, ("partition in state %d", slsp->slsp_status));

	DEBUG("Stopped checkpointing");

out:
	/* Drop the reference we got for the SLS process. */
	slsp_deref(slsp);

	if (procset != NULL) {
		/* Release any process references gained. */
		KVSET_FOREACH_POP(procset, p)
		PRELE(p);
		slskv_destroy(procset);
	}

	/* Release the module reference. */
	sls_finishop();

	kthread_exit();
}
