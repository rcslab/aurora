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
#include <sys/refcount.h>
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
#include "sls_prefault.h"
#include "sls_table.h"
#include "sls_vm.h"

#define SLSCKPT_ZONEWARM (64)

uma_zone_t slsckpt_zone;
struct slspart_serial ssparts[SLS_OIDRANGE];

static int
slsckpt_init(struct slsckpt_data *sckpt)
{
	int error;

	error = slskv_create(&sckpt->sckpt_rectable);
	if (error != 0)
		goto error;

	error = slskv_create(&sckpt->sckpt_shadowtable);
	if (error != 0)
		goto error;

	error = slsset_create(&sckpt->sckpt_vntable);
	if (error != 0)
		goto error;

	sckpt->sckpt_meta = sbuf_new_auto();
	if (sckpt->sckpt_meta == NULL)
		goto error;

	sckpt->sckpt_dataid = sbuf_new_auto();
	if (sckpt->sckpt_dataid == NULL)
		goto error;

	refcount_init(&sckpt->sckpt_refcount, 1);

	return (0);

error:

	if (sckpt->sckpt_dataid != NULL)
		sbuf_delete(sckpt->sckpt_dataid);

	if (sckpt->sckpt_meta != NULL)
		sbuf_delete(sckpt->sckpt_meta);

	if (sckpt != NULL) {
		slskv_destroy(sckpt->sckpt_vntable);
		slskv_destroy(sckpt->sckpt_shadowtable);
		slskv_destroy(sckpt->sckpt_rectable);
	}

	return (error);
}

static void
slsckpt_fini(struct slsckpt_data *sckpt)
{
	KASSERT(
	    sckpt->sckpt_refcount == 0, ("deinitializing referenced sckpt"));

	sbuf_delete(sckpt->sckpt_dataid);
	sbuf_delete(sckpt->sckpt_meta);
	slsset_destroy(sckpt->sckpt_vntable);
	slskv_destroy(sckpt->sckpt_shadowtable);
	slskv_destroy(sckpt->sckpt_rectable);
	free(sckpt, M_SLSMM);
}

void
slsckpt_clear(struct slsckpt_data *sckpt)
{
	vm_object_t obj, shadow;
	struct sls_record *rec;
	struct vnode *vp;
	uint64_t slsid;

	/* Collapse all objects now being backed. */
	KV_FOREACH_POP(sckpt->sckpt_shadowtable, obj, shadow)
	{
		vm_object_deallocate(obj);
	}

	/* Release all held vnodes. */
	KVSET_FOREACH_POP(sckpt->sckpt_vntable, vp)
	{
		if (vp->v_mount == slos.slsfs_mount)
			slspre_vector_populated(INUM(SLSVP(vp)), vp->v_object);
		vrele(vp);
	}

	KV_FOREACH_POP(sckpt->sckpt_rectable, slsid, rec)
	sls_record_destroy(rec);

	sbuf_clear(sckpt->sckpt_dataid);
	sbuf_clear(sckpt->sckpt_meta);
}

int
slsckpt_alloc(struct slspart *slsp, struct slsckpt_data **sckptp)
{
	struct slsckpt_data *sckpt;
	int error;

	sckpt = slsp->slsp_blanksckpt;
	slsp->slsp_blanksckpt = NULL;

	if (sckpt == NULL)
		sckpt = malloc(sizeof(*sckpt), M_SLSMM, M_WAITOK | M_ZERO);

	error = slsckpt_init(sckpt);
	if (error != 0) {
		free(sckpt, M_SLSMM);
		return (error);
	}

	memcpy(&sckpt->sckpt_attr, &slsp->slsp_attr, sizeof(sckpt->sckpt_attr));
	*sckptp = sckpt;

	return (0);
}

void
slsckpt_hold(struct slsckpt_data *sckpt)
{
	KASSERT(sckpt->sckpt_refcount > 0, ("holding unreferenced sckpt"));
	refcount_acquire(&sckpt->sckpt_refcount);
}

void
slsckpt_drop(struct slsckpt_data *sckpt)
{
	bool release;

	KASSERT(sckpt->sckpt_refcount > 0, ("dropping unreferenced sckpt"));

	release = refcount_release(&sckpt->sckpt_refcount);

	/* Free if that was the last reference. */
	if (release) {
		slsckpt_clear(sckpt);
		slsckpt_fini(sckpt);
	}
}

int
slsckpt_addrecord(
    struct slsckpt_data *sckpt, uint64_t slsid, struct sbuf *sb, uint64_t type)
{
	struct sls_record *rec;
	size_t len;
	int error;

	rec = sls_getrecord(sb, slsid, type);
	if (rec == NULL)
		return (ENOMEM);

	/* XXX Avoid this altogether for metadata records, keep a hashtable or
	 * something. */
	error = slskv_add(sckpt->sckpt_rectable, slsid, (uintptr_t)rec);
	if (error != 0) {
		sls_record_destroy(rec);
		return (error);
	}

	/* Only create */
	if (type == SLOSREC_VMOBJ) {
		error = sbuf_bcat(sckpt->sckpt_dataid, &slsid, sizeof(slsid));
		return (error);
	}

	/*
	 * Append the metadata record, length, and data.
	 */
	error = sbuf_bcat(sckpt->sckpt_meta, rec, sizeof(*rec));
	if (error != 0)
		return (error);

	len = sbuf_len(rec->srec_sb);
	error = sbuf_bcat(sckpt->sckpt_meta, &len, sizeof(len));
	if (error != 0)
		return (error);

	error = sbuf_bcat(
	    sckpt->sckpt_meta, sbuf_data(rec->srec_sb), sbuf_len(rec->srec_sb));
	if (error != 0)
		return (error);

	return (0);
}

struct slspart *
slsp_find_locked(uint64_t oid)
{
	struct slspart *slsp;
	SLS_ASSERT_LOCKED();

	/* Get the partition if it exists. */
	if (slskv_find(slsm.slsm_parts, oid, (uintptr_t *)&slsp) != 0)
		return (NULL);

	/* We found the process, take a reference to it. */
	slsp_ref(slsp);

	return (slsp);
}

/* Find a process in the SLS with the given PID. */
struct slspart *
slsp_find(uint64_t oid)
{
	struct slspart *slsp;

	/* Use the SLS module lock to serialize accesses to the partition table.
	 */
	SLS_LOCK();
	slsp = slsp_find_locked(oid);
	SLS_UNLOCK();

	return (slsp);
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
	int error;

	/* We cannot checkpoint the kernel. */
	if (pid == 0) {
		DEBUG("Trying to attach the kernel to a partition");
		return (EINVAL);
	}

	/* Make sure the PID isn't in the SLS already. */
	if (slsset_find(slsm.slsm_procs, pid) == 0)
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

static int
slsp_init_filename(struct slspart *slsp, struct vnode *vp)
{
	struct thread *td = curthread;
	char *freepath = NULL;
	char *fullpath = "";
	int error;

	error = vn_fullpath(td, vp, &fullpath, &freepath);
	if (error != 0) {
		free(freepath, M_TEMP);
		return (error);
	}

	strncpy(slsp->slsp_name, fullpath, PATH_MAX);
	free(freepath, M_TEMP);

	return (0);
}

static int
slsp_init_rcvname(struct slspart *slsp, int fd)
{
	socklen_t alen = PATH_MAX - sizeof(alen);
	struct thread *td = curthread;
	struct sockaddr *sa;
	int error;

	error = kern_getsockname(td, fd, &sa, &alen);
	if (error != 0)
		return (error);

	memcpy(slsp->slsp_name, &alen, sizeof(alen));
	memcpy(&slsp->slsp_name[sizeof(alen)], sa, alen);
	free(sa, M_SONAME);

	return (0);
}

static int
slsp_init_sndname(struct slspart *slsp, int fd)
{
	socklen_t alen = PATH_MAX - sizeof(alen);
	struct thread *td = curthread;
	struct sockaddr *sa;
	int error;

	error = kern_getpeername(td, fd, &sa, &alen);
	if (error != 0)
		return (error);

	memcpy(slsp->slsp_name, &alen, sizeof(alen));
	memcpy(&slsp->slsp_name[sizeof(alen)], sa, alen);
	free(sa, M_SONAME);

	return (0);
}

/*
 * Create a new struct slspart to be entered into the SLS.
 */
static int
slsp_init(uint64_t oid, struct sls_attr attr, int fd, struct slspart **slspp)
{
	struct thread *td = curthread;
	struct slspart *slsp = NULL;
	struct file *fp;
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
	slsp->slsp_backend = NULL;
	slsp->slsp_blanksckpt = NULL;

	/* Create the set of held processes. */
	error = slsset_create(&slsp->slsp_procs);
	if (error != 0)
		goto error;

	fp = (fd >= 0) ? FDTOFP(td->td_proc, fd) : NULL;

	switch (slsp->slsp_target) {
	case SLS_FILE:
		error = slsp_init_filename(slsp, fp->f_vnode);
		if (error != 0)
			break;

		slsp->slsp_backend = fp->f_vnode;
		vref(slsp->slsp_backend);

		break;

	case SLS_SOCKSND:
		error = slsp_init_sndname(slsp, fd);
		break;

	case SLS_SOCKRCV:
		error = slsp_init_rcvname(slsp, fd);
		if (error != 0)
			break;

		if (!fhold((struct file *)fp)) {
			error = EBADF;
			goto error;
		}

		slsp->slsp_backend = fp;
		break;

	case SLS_MEM:
	case SLS_OSD:
		/* XXX Add a name for the OSD partitions. */
		break;

	default:
		panic("invalid partition target %d\n", slsp->slsp_target);
	}

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
	struct thread *td = curthread;
	int backend;

	/* Remove all processes currently in the partition from the SLS. */
	slsp_detachall(slsp);

	if (slsp->slsp_blanksckpt != NULL) {
		slsckpt_drop(slsp->slsp_blanksckpt);
		slsp->slsp_blanksckpt = NULL;
	}

	/* Destroy the synchronization mutexes/condition variables. */
	mtx_assert(&slsp->slsp_syncmtx, MA_NOTOWNED);
	cv_destroy(&slsp->slsp_synccv);
	mtx_destroy(&slsp->slsp_syncmtx);

	mtx_assert(&slsp->slsp_epochmtx, MA_NOTOWNED);
	cv_destroy(&slsp->slsp_epochcv);
	mtx_destroy(&slsp->slsp_epochmtx);

	/* Destroy the proc bookkeeping structure. */
	slsset_destroy(slsp->slsp_procs);

	if (slsp->slsp_sckpt != NULL)
		slsckpt_drop(slsp->slsp_sckpt);

	backend = slsp->slsp_attr.attr_target;
	switch (backend) {
	case SLS_FILE:
		vrele((struct vnode *)slsp->slsp_backend);
		slsp->slsp_backend = NULL;
		break;

	case SLS_SOCKRCV:
		fdrop((struct file *)slsp->slsp_backend, td);
		slsp->slsp_backend = NULL;
		break;

	case SLS_SOCKSND:
	case SLS_MEM:
	case SLS_OSD:
		break;

	default:
		printf("BUG: Invalid backend %d\n", backend);
	}

	free(slsp, M_SLSMM);
}

/* Add a partition with unique ID oid to the SLS. */
int
slsp_add(uint64_t oid, struct sls_attr attr, int fd, struct slspart **slspp)
{
	struct slspart *slsp;
	int error;

	/*
	 * Try to find if we already have added the process
	 * to the SLS, if so we can't add it again.
	 */
	slsp = slsp_find(oid);
	if (slsp != NULL) {
		/* We got a ref to the process with slsp_find, release it. */

		slsp_deref(slsp);
		return (EINVAL);
	}

	/* If we didn't find it, create one. */
	error = slsp_init(oid, attr, fd, &slsp);
	if (error != 0)
		return (error);

	/* Add the partition to the table. */
	error = slskv_add(slsm.slsm_parts, oid, (uintptr_t)slsp);
	if (error != 0) {
		slsp_fini(slsp);
		return (error);
	}

	/* Export the partition to the caller. */
	if (slspp != NULL)
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

}

void
slsp_ref(struct slspart *slsp)
{
	atomic_add_int(&slsp->slsp_refcount, 1);
}

void
slsp_deref_locked(struct slspart *slsp)
{
	SLS_ASSERT_LOCKED();
	atomic_add_int(&slsp->slsp_refcount, -1);

	/*
	 * Deleting a partition is a heavyweight
	 * operation during which we sleep. Only
	 * hold the lock until we remove it from
	 * the hash table. Destroying the partition
	 * safe since there is no way another thread
	 * can get a reference.
	 */
	if (slsp->slsp_refcount == 0) {
		slskv_del(slsm.slsm_parts, slsp->slsp_oid);
		SLS_UNLOCK();
		slsp_fini(slsp);
		SLS_LOCK();
	}
}

void
slsp_deref(struct slspart *slsp)
{
	/*
	 * Lock the module, because dropping the refcount to 0 and
	 * removing the partition from the table should be done
	 * atomically.
	 */
	SLS_LOCK();
	slsp_deref_locked(slsp);
	SLS_UNLOCK();
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

	/* Update the on-disk representation, it will be sent out automatically.
	 */
	ssparts[slsp->slsp_oid].sspart_epoch = slsp->slsp_epoch;

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

/*
 * Return whether we should restore from memory or the SLOS.
 */
bool
slsp_rest_from_mem(struct slspart *slsp)
{
	if (slsp->slsp_sckpt == NULL)
		return (false);

	if (slsp->slsp_target == SLS_MEM)
		return (true);

	if (slsp->slsp_target == SLS_OSD)
		return (SLSP_CACHEREST(slsp));

	return (false);
}

bool
slsp_restorable(struct slspart *slsp)
{
	KASSERT(slsp->slsp_amplification > 0,
	    ("invalid amplification %lx", slsp->slsp_amplification));
	if (slsp->slsp_amplification > 1)
		return (false);

	return (true);
}

/* Deserialize the already read on-disk partitions. */
int
sslsp_deserialize(void)
{
	struct slspart *slsp;
	int error;
	int i;

	for (i = 0; i < SLS_OIDRANGE; i++) {
		if (!ssparts[i].sspart_valid)
			continue;

		/* Create the in-memory representation. */
		error = slsp_add(
		    ssparts[i].sspart_oid, ssparts[i].sspart_attr, -1, &slsp);
		if (error != 0)
			return (error);

		/* Fill in Metropolis state. */
		slsp->slsp_metr.slsmetr_proc =
		    ssparts[slsp->slsp_oid].sspart_proc;
		slsp->slsp_metr.slsmetr_td = ssparts[slsp->slsp_oid].sspart_td;
		slsp->slsp_metr.slsmetr_flags =
		    ssparts[slsp->slsp_oid].sspart_flags;
		slsp->slsp_metr.slsmetr_sockid =
		    ssparts[slsp->slsp_oid].sspart_sockid;
	}

	return (0);
}
