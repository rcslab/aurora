#include <sys/param.h>
#include <sys/kthread.h>
#include <sys/taskqueue.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_vm.h"
#include "sls_vmobject.h"

uint64_t sls_memsnap_attempted;
uint64_t sls_memsnap_done;

SDT_PROBE_DEFINE1(sls, , slsckpt_dataregion_dump, , "char *");
SDT_PROBE_DEFINE1(sls, , slsckpt_dataregion_fillckpt, , "char *");
SDT_PROBE_DEFINE1(sls, , slsckpt_dataregion, , "char *");
SDT_PROBE_DEFINE0(sls, , , enter);
SDT_PROBE_DEFINE0(sls, , , cow);
SDT_PROBE_DEFINE0(sls, , , write);
SDT_PROBE_DEFINE0(sls, , , wait);
SDT_PROBE_DEFINE0(sls, , , cleanup);

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
	error = slsckpt_dataregion_getvm(slsp, p, addr, sckpt_data, &entry,
	    &obj);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , slsckpt_dataregion_fillckpt, , "Getting the object");
	KASSERT(OBJT_ISANONYMOUS(obj),
	    ("getting metadata for non anonymous obj"));

	/* Only objects referenced only by the entry can be shadowed. */
	if (obj->ref_count > 1)
		return (EINVAL);

	/*Get the metadata of the VM object. */
	error = slsvmobj_checkpoint(obj, sckpt_data);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , slsckpt_dataregion_fillckpt, ,
	    "Object checkpointing");
	/* Get the data and shadow it for the entry. */
	error = slsvm_entry_shadow(p, sckpt_data->sckpt_shadowtable, entry,
	    slsp_isfullckpt(slsp), true);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , slsckpt_dataregion_fillckpt, , "Object shadowing");

	return (0);
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
		error = slskv_add(slsp->slsp_sckpt->sckpt_rectable, slsid,
		    (uintptr_t)rec);
		KASSERT(error == 0, ("Could not add new record"));
	}

	/* Collapse the new table into the old one. */
	slsvm_objtable_collapsenew(slsp->slsp_sckpt->sckpt_shadowtable,
	    sckpt->sckpt_shadowtable);
	slsckpt_clear(sckpt);
	slsp->slsp_blanksckpt = sckpt;
}

static void
slsckpt_dataregion_cleanup(struct slsckpt_data *sckpt_data,
    struct slspart *slsp, uint64_t nextepoch)
{
	int stateerr;

	/*
	 * We compact before turning the checkpoint available, because we modify
	 * the partition's current checkpoint data.
	 */
	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_mode == SLS_DELTA))
		slsckpt_compact_single(slsp, sckpt_data);
	else
		slsckpt_drop(sckpt_data);

	slsp_epoch_advance(slsp, nextepoch);

	stateerr = slsp_setstate(slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE,
	    false);
	KASSERT(stateerr == 0, ("partition not in ckpt state"));

	slsp_deref(slsp);
}

struct slsckpt_dataregion_dump_args {
	struct slsckpt_data *sckpt_data;
	struct slspart *slsp;
	uint64_t nextepoch;
};

static __noinline void
slsckpt_dataregion_dump(struct slsckpt_data *sckpt_data, struct slspart *slsp,
    uint64_t nextepoch)
{
	int error;

	if (slsp->slsp_target == SLS_OSD) {
		error = sls_write_slos_dataregion(sckpt_data);
		if (error != 0) {
			printf("%s: %d\n", __func__, __LINE__);
			DEBUG1("slsckpt_initio failed with %d", error);
		}
	}

	SDT_PROBE0(sls, , , write);
	/* Drain the taskqueue, ensuring all IOs have hit the disk. */
	if (slsp->slsp_target == SLS_OSD) {
		taskqueue_drain_all(slos.slos_tq);
		/* XXX Using MNT_WAIT is causing a deadlock right now. */
		VFS_SYNC(slos.slsfs_mount,
		    (sls_vfs_sync != 0) ? MNT_WAIT : MNT_NOWAIT);
	}

	SDT_PROBE0(sls, , , wait);
	slsckpt_dataregion_cleanup(sckpt_data, slsp, nextepoch);

	SDT_PROBE0(sls, , , cleanup);

	sls_finishop();
}

static __noinline void
slsckpt_dataregion_dumptask(void *ctx, int __unused pending)
{
	union slstable_taskctx *taskctx = (union slstable_taskctx *)ctx;
	struct slstable_msnapctx *msnapctx = &taskctx->msnap;
	struct slsckpt_data *sckpt_data = msnapctx->sckpt;
	uint64_t nextepoch = msnapctx->nextepoch;
	struct slspart *slsp = msnapctx->slsp;

	uma_zfree(slstable_task_zone, taskctx);
	slsckpt_dataregion_dump(sckpt_data, slsp, nextepoch);
}

int
slsckpt_dataregion(struct slspart *slsp, struct proc *p, vm_ooffset_t addr,
    uint64_t *nextepoch)
{
	struct slstable_msnapctx *msnapctx;
	struct slsckpt_data *sckpt = NULL;
	union slstable_taskctx *taskctx;
	int stateerr, error;

	sls_memsnap_attempted += 1;

	/*
	 * Unless doing full checkpoints (which are there just for benchmarking
	 * purposes), we have to have checkpointed the whole partition before
	 * checkpointing individual regions.
	 */
	if ((slsp->slsp_target == SLS_MEM) || (slsp->slsp_mode == SLS_DELTA)) {
		if (slsp->slsp_sckpt == NULL) {
			sls_finishop();
			return (EINVAL);
		}
	}

	/* Wait till we can start checkpointing. */
	if (slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_CHECKPOINTING, true) !=
	    0) {

		/* Once a partition is detached its state cannot change.  */
		KASSERT(slsp->slsp_status == SLSP_DETACHED,
		    ("Blocking slsp_setstate() on live partition failed"));

		sls_finishop();

		/* Tried to checkpoint a removed partition. */
		return (EINVAL);
	}

	/*
	 * Once a process is in a partition it is there forever, so there can be
	 * no races with the call below.
	 */
	if (!slsp_proc_in_part(slsp, p)) {
		error = EINVAL;
		goto error_single;
	}

	SDT_PROBE0(sls, , , enter);

	error = slsckpt_alloc(slsp, &sckpt);
	if (error != 0)
		goto error;

	/* Single thread to avoid races with other threads. */
	PROC_LOCK(p);
	thread_single(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	/* Add the data and metadata. This also shadows the object. */
	error = slsckpt_dataregion_fillckpt(slsp, p, addr, sckpt);
	if (error != 0) {
		error = EINVAL;
		goto error;
	}

	/* Preadvance the epoch, the kthread will advance it. */
	*nextepoch = slsp_epoch_preadvance(slsp);

	/* The process can continue while we are dumping. */
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	SDT_PROBE0(sls, , , cow);

	if (!SLSATTR_ISASYNCSNAP(slsp->slsp_attr)) {
		slsckpt_dataregion_dump(sckpt, slsp, *nextepoch);
		sls_memsnap_done += 1;
		return (0);
	}

	taskctx = uma_zalloc(slstable_task_zone, M_WAITOK);
	msnapctx = &taskctx->msnap;
	msnapctx->slsp = slsp;
	msnapctx->sckpt = sckpt;
	msnapctx->nextepoch = *nextepoch;
	TASK_INIT(&msnapctx->tk, 0, &slsckpt_dataregion_dumptask,
	    &msnapctx->tk);
	taskqueue_enqueue(slsm.slsm_tabletq, &msnapctx->tk);

	sls_memsnap_done += 1;

	return (0);

error:
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

error_single:
	stateerr = slsp_setstate(slsp, SLSP_CHECKPOINTING, SLSP_AVAILABLE,
	    false);
	KASSERT(stateerr == 0, ("partition not in ckpt state"));

	slsckpt_drop(sckpt);

	/* Remove the reference taken by the initial ioctl call. */
	slsp_deref(slsp);

	sls_finishop();

	return (error);
}
