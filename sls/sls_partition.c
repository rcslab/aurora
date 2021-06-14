#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <machine/reg.h>

#include <slos.h>
#include <slos_inode.h>
#include <sls_data.h>

#include "debug.h"
#include "sls_data.h"
#include "sls_internal.h"
#include "sls_partition.h"
#include "sls_table.h"
#include "sls_vm.h"

/* XXX Turn this into a zone, this is gonna be a bottleneck for region ckpts. */
int
slsckpt_create(struct slsckpt_data **sckpt_datap, struct sls_attr *attr)
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

	error = slskv_create(&sckpt_data->sckpt_vntable);
	if (error != 0)
		goto error;

	memcpy(&sckpt_data->sckpt_attr, attr, sizeof(sckpt_data->sckpt_attr));

	*sckpt_datap = sckpt_data;

	return (0);

error:

	if (sckpt_data != NULL) {
		slskv_destroy(sckpt_data->sckpt_vntable);
		slskv_destroy(sckpt_data->sckpt_objtable);
		slskv_destroy(sckpt_data->sckpt_rectable);
	}

	free(sckpt_data, M_SLSMM);

	return (error);
}

void
slsckpt_destroy(struct slsckpt_data *sckpt_data, struct slsckpt_data *sckpt_new)
{
	struct slskv_table *newtable;
	struct vnode *vp;

	if (sckpt_data == NULL)
		return;

	DEBUG("Destroying stored checkpoint");
	/*
	 * Collapse all objects created for the checkpoint. If delta
	 * checkpointing, update the new table's keys to be the backers of
	 * the shadows being destroyed here, instead of the shadows themselves.
	 */
	newtable = (sckpt_new != NULL) ? sckpt_new->sckpt_objtable : NULL;
	slsvm_objtable_collapse(sckpt_data->sckpt_objtable, newtable);
	slskv_destroy(sckpt_data->sckpt_objtable);

	/* Release all held vnodes. */
	KVSET_FOREACH_POP(sckpt_data->sckpt_vntable, vp)
	vrele(vp);
	slsset_destroy(sckpt_data->sckpt_vntable);

	sls_free_rectable(sckpt_data->sckpt_rectable);

	free(sckpt_data, M_SLSMM);
}

/* Find a process in the SLS with the given PID. */
struct slspart *
slsp_find(uint64_t oid)
{
	struct slspart *slsp;

	/* XXX LOCKING - See slsp_deref */
	/* Get the partition if it exists. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *)&slsp) != 0)
		return (NULL);

	/* We found the process, take a reference to it. */
	slsp_ref(slsp);

	return slsp;
}

bool
slsp_hasproc(struct slspart *slsp, pid_t pid)
{
	uint64_t oid;

	if (slskv_find(slsm.slsm_procs, pid, &oid) != 0)
		return (false);

	KASSERT(slsp->slsp_oid == oid, ("Process is in the wrong partition"));
	return (true);
}

/* Attach a process to the partition. */
int
slsp_attach(uint64_t oid, pid_t pid)
{
	struct slspart *slsp;
	uintptr_t oldoid;
	int error;

	/* We cannot checkpoint the kernel. */
	if (pid == 0) {
		DEBUG("Trying to attach the kernel to a partition");
		return (EINVAL);
	}

	/* Make sure the PID isn't in the SLS already. */
	if (slskv_find(slsm.slsm_procs, pid, &oldoid) == 0)
		return (EINVAL);

	/* Make sure the partition actually exists. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *)&slsp) != 0)
		return (EINVAL);

	error = slskv_add(slsm.slsm_procs, pid, (uintptr_t)oid);
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
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *)&slsp) != 0)
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
	slsp->slsp_status = SLSP_AVAILABLE;
	slsp->slsp_epoch = SLSPART_EPOCHINIT;
	slsp->slsp_nextepoch = slsp->slsp_epoch + 1;
	/* Create the set of held processes. */
	error = slsset_create(&slsp->slsp_procs);
	if (error != 0)
		goto error;

	mtx_init(&slsp->slsp_syncmtx, "slssync", NULL, MTX_DEF);
	cv_init(&slsp->slsp_synccv, "slssync");

	mtx_init(&slsp->slsp_epochmtx, "slsepoch", NULL, MTX_DEF);
	cv_init(&slsp->slsp_epochcv, "slsepoch");

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

	/* Destroy the synchronization mutexes/condition variables. */
	mtx_assert(&slsp->slsp_syncmtx, MA_NOTOWNED);
	cv_destroy(&slsp->slsp_synccv);
	mtx_destroy(&slsp->slsp_syncmtx);

	mtx_assert(&slsp->slsp_epochmtx, MA_NOTOWNED);
	cv_destroy(&slsp->slsp_epochcv);
	mtx_destroy(&slsp->slsp_epochmtx);

	/*
	 * XXX TEMP Remove the partition from the SLOS. This is due to us not
	 * importing existing partitions from the SLOS, and thus having
	 * collisions on the partition numbers. If we implement sls ps, the user
	 * will be able to find which IDs are available.
	 */
	slos_iremove(&slos, slsp->slsp_oid);

	/* Destroy the proc bookkeeping structure. */
	slsset_destroy(slsp->slsp_procs);

	/* Remove any references to VM objects we may have. */
	if (slsp->slsp_objects != NULL) {
		printf("Collapsing the table\n");
		slsvm_objtable_collapse(slsp->slsp_objects, NULL);
		slskv_destroy(slsp->slsp_objects);
	}

	if (slsp->slsp_sckpt != NULL)
		slsckpt_destroy(slsp->slsp_sckpt, NULL);

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
		/* We got a reference to the process with slsp_find, release it.
		 */

		slsp_deref(slsp);
		return (EINVAL);
	}

	/* If we didn't find it, create one. */
	error = slsp_init(oid, attr, &slsp);
	if (error != 0)
		return (error);

	/* Add the partition to the table. */
	error = slskv_add(slsm.slsm_parts, oid, (uintptr_t)slsp);
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
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *)&slsp) != 0)
		return;

	/*
	 * XXX When destroying a partition, kill all processes associated with
	 * it. These may be Aurora processes, Metropolis processes, or restored
	 * processes. A partition is a long lived entity that spans reboot, so
	 * destroying it should be a very rare event.
	 */

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
	while (slskv_pop(slsm.slsm_parts, &oid, (uintptr_t *)&slsp) == 0)
		slsp_fini(slsp);

	/* Remove all processes from the global table.  */
	if (slsm.slsm_procs != NULL) {
		slskv_destroy(slsm.slsm_procs);
		slsm.slsm_procs = NULL;
	}

	if (slsm.slsm_parts != NULL) {
		slskv_destroy(slsm.slsm_parts);
		slsm.slsm_parts = NULL;
	}
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

/*
 * Epoch advancement functions. Threads that want to advance the epoch may need
 * to wait for threads that previously expressed their intent to advance the
 * epoch to be done. We implement this by taking a ticket to advance the lock,
 * then waiting for it to be called to actually do it.
 */

uint64_t
slsp_epoch_preadvance(struct slspart *slsp)
{
	uint64_t next_epoch;

	mtx_lock(&slsp->slsp_epochmtx);

	KASSERT(slsp->slsp_nextepoch != UINT64_MAX, ("Epoch overflow"));
	next_epoch = slsp->slsp_nextepoch;
	slsp->slsp_nextepoch += 1;

	KASSERT(next_epoch > slsp->slsp_epoch, ("Got a passed epoch"));

	/* No need to signal anyone, we didn't actually change the epoch. */
	mtx_unlock(&slsp->slsp_epochmtx);

	return (next_epoch);
}

void
slsp_epoch_advance(struct slspart *slsp, uint64_t next_epoch)
{
	mtx_lock(&slsp->slsp_epochmtx);
	while (slsp->slsp_epoch + 1 != next_epoch)
		cv_wait(&slsp->slsp_epochcv, &slsp->slsp_epochmtx);

	KASSERT(slsp->slsp_epoch != UINT64_MAX, ("Epoch overflow"));
	slsp->slsp_epoch += 1;
	cv_broadcast(&slsp->slsp_epochcv);

	KASSERT(slsp->slsp_epoch == next_epoch, ("Unexpected epoch"));
	mtx_unlock(&slsp->slsp_epochmtx);
}

int
slsp_waitfor(struct slspart *slsp)
{
	int error;

	mtx_assert(&slsp->slsp_syncmtx, MA_OWNED);

	while (!slsp->slsp_syncdone)
		cv_wait(&slsp->slsp_synccv, &slsp->slsp_syncmtx);
	slsp->slsp_syncdone = false;
	error = slsp->slsp_retval;

	mtx_unlock(&slsp->slsp_syncmtx);

	return (error);
}

void
slsp_signal(struct slspart *slsp, int retval)
{
	mtx_lock(&slsp->slsp_syncmtx);
	slsp->slsp_syncdone = true;
	slsp->slsp_retval = retval;
	cv_signal(&slsp->slsp_synccv);
	mtx_unlock(&slsp->slsp_syncmtx);
}

int
slsp_setstate(struct slspart *slsp, int curstate, int nextstate, bool sleep)
{
	struct mtx *mtxp;

	KASSERT(curstate != SLSP_DETACHED,
	    ("Trying to change detached partition's state"));

	mtxp = mtx_pool_find(mtxpool_sleep, slsp);
	mtx_lock(mtxp);
	while (slsp->slsp_status != curstate) {
		if (!sleep) {
			mtx_unlock(mtxp);
			return (EBUSY);
		}

		/* This is urgent, wake us up as if we were a realtime task. */
		mtx_sleep(slsp, mtxp, PI_DULL, "slspstate", 0);

		/* If the partition is detached we can't do anything.  */
		if (slsp->slsp_status == SLSP_DETACHED) {
			mtx_unlock(mtxp);
			return (EINVAL);
		}
	}

	slsp->slsp_status = nextstate;
	wakeup(slsp);
	mtx_unlock(mtxp);

	return (0);
}

int
slsp_getstate(struct slspart *slsp)
{
	return (slsp->slsp_status);
}

bool
slsp_rest_from_mem(struct slspart *slsp)
{
	if (slsp->slsp_sckpt == NULL)
		return (false);

	return (slsp->slsp_target == SLS_MEM);
}
