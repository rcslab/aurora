#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitstring.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_ioctl.h"
#include "sls_kv.h"
#include "sls_metropolis.h"
#include "sls_pager.h"
#include "sls_syscall.h"
#include "sls_table.h"
#include "sls_vm.h"

/* XXX Rename to M_SLS. */
MALLOC_DEFINE(M_SLSMM, "sls", "SLS");
MALLOC_DEFINE(M_SLSREC, "slsrec", "SLSREC");

SDT_PROVIDER_DEFINE(sls);

extern int sls_objprotect;

struct sls_metadata slsm;
struct sysctl_ctx_list aurora_ctx;

static void
sls_register_fault(vm_map_entry_t entry)
{
	vm_object_t object;

	object = entry->object.vm_object;
	if (object == NULL)
		return;

	if (object->type != OBJT_VNODE)
		return;

	if (((struct vnode *)object->handle)->v_mount != slos.slsfs_mount)
		return;
}

/*
 * XXXHACK: Populate the zones used in the SLS to avoid slab allocations. We
 * tend to unload and load the module a lot, in order to ensure there is no
 * leftover state influencing our benchmark numbers. This, however, means that
 * we always use the zones cold. By calling sls_zonewarm(), we warm up the zones
 * to avoid these performance hits.
 */
int
sls_zonewarm(uma_zone_t zone)
{
	void **warmdata;
	int error = 0;
	int i;

	/* Warm up the zone by creating and then freeing structures. */
	warmdata = malloc(
	    sizeof(*warmdata) * SLS_ZONEWARM, M_SLSMM, M_WAITOK | M_ZERO);

	/* Initialize a certain number of items in the zone. */
	for (i = 0; i < SLS_ZONEWARM; i++) {
		warmdata[i] = uma_zalloc(zone, M_NOWAIT);
		if (warmdata[i] == NULL) {
			error = ENOMEM;
			break;
		}
	}

	/* Free them. The items are still warm after freeing. */
	for (i = 0; i < SLS_ZONEWARM; i++)
		uma_zfree(zone, warmdata[i]);
	free(warmdata, M_SLSMM);

	return (error);
}

static bool
sls_proc_insls(struct proc *p)
{
	struct proc *aurp;

	SLS_ASSERT_LOCKED();

	/* Check if we're already in Aurora. */
	LIST_FOREACH (aurp, &slsm.slsm_plist, p_aurlist) {
		if (aurp == p)
			return (true);
	}

	return (false);
}

/* Add a process into Aurora. */
void
sls_procadd(uint64_t oid, struct proc *p, bool metropolis)
{
	SLS_ASSERT_LOCKED();
	PROC_LOCK_ASSERT(p, MA_OWNED);

	/* Check if we're already in Aurora. */
	if (sls_proc_insls(p))
		return;

	p->p_auroid = oid;
	p->p_sysent = &slssyscall_sysvec;
	if (metropolis)
		p->p_sysent = &slsmetropolis_sysvec;

	LIST_INSERT_HEAD(&slsm.slsm_plist, p, p_aurlist);
}

/* Remove a process from Aurora. */
void
sls_procremove(struct proc *p)
{

	SLS_ASSERT_LOCKED();
	PROC_LOCK_ASSERT(p, MA_OWNED);

	/* Check if we're already in Aurora. */
	if (!sls_proc_insls(p))
		return;

	LIST_REMOVE(p, p_aurlist);
	p->p_auroid = 0;

	return;
}

static int
sls_prockillall(void)
{
	struct proc *p, *tmp;

	/* Wait for all the children to be done. */
	LIST_FOREACH_SAFE (p, &slsm.slsm_plist, p_aurlist, tmp) {
		/* Kill the process and wait for it. */
		PROC_LOCK(p);
		kern_psignal(p, SIGKILL);
		PROC_UNLOCK(p);

		while (sls_proc_insls(p))
			cv_wait(&slsm.slsm_exitcv, &slsm.slsm_mtx);

		/*
		 * There is no need to remove the processes from the list, they
		 * remove themselves when exiting.
		 */
	}

	return (0);
}

/*
 * Called by the initial Metropolis function. Replaces the syscall vector with
 * one that has overloaded execve() and accept() calls. The accept() call is
 * really what we are after here, since it is used to demarcate the point in
 * which we checkpoint the function. We also overload execve(), which normally
 * override the system call vector, and fork(), so that new processes
 * dynamically enter themselves into the partition.
 *
 * The initial process entering Metropolis mode is NOT in Metropolis. This
 * allows us to use it to set up a new environment for each new Metropolis
 * instance.
 */
static int
sls_metropolis(struct sls_metropolis_args *args)
{
	struct proc *p = curthread->td_proc;
	struct slspart *slsp;

	SLS_LOCK();
	if (SLS_EXITING()) {
		SLS_UNLOCK();
		return (ENODEV);
	}

	/* Check if the partition actually exists. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL) {
		SLS_UNLOCK();
		return (EINVAL);
	}

	slsp_deref(slsp);

	/* Add the process in Aurora. */
	PROC_LOCK(p);
	/* The process isn't being checkpointed, but is in the SLS. */
	sls_procadd(args->oid, p, true);
	PROC_UNLOCK(p);
	SLS_UNLOCK();

	return (0);
}

/*
 * Create a new Metropolis process, handing it off a connected socket in the
 * process.
 */
static int
sls_metropolis_spawn(struct sls_metropolis_spawn_args *args)
{
	struct sls_restore_args rest_args;
	struct thread *td = curthread;
	struct slsmetr *slsmetr;
	struct slspart *slsp;
	int error;

	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (EINVAL);
	slsmetr = &slsp->slsp_metr;

	/*
	 * Get the new connected socket, save it in the partition. This call
	 * also fills in any information the restore process might need.
	 */
	error = kern_accept4(td, args->s, &slsmetr->slsmetr_sa,
	    &slsmetr->slsmetr_namelen, slsmetr->slsmetr_flags,
	    &slsmetr->slsmetr_sockfp);
	slsp_deref(slsp);
	if (error != 0)
		return (error);

	rest_args = (struct sls_restore_args) {
		.oid = args->oid,
		.daemon = 0,
		.rest_stopped = 0,
	};

	/* Fully restore the partition. */
	return (sls_restore(&rest_args));
}

/*
 * Start checkpointing a partition. If a checkpointing period
 * has been set, then the partition gets periodically
 * checkpointed, otherwise it's a one-off.
 */
int
sls_checkpoint(struct sls_checkpoint_args *args)
{
	struct sls_checkpointd_args *ckptd_args;
	struct proc *p = curproc;
	struct slspart *slsp;
	uint64_t nextepoch;
	int error = 0;

	/* Take another reference for the worker thread. */
	if (sls_startop(true) != 0)
		return (EBUSY);

	/* Get the partition to be checkpointed. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL) {
		sls_finishop();
		return (EINVAL);
	}

	/* Set up the arguments. */
	ckptd_args = malloc(sizeof(*ckptd_args), M_SLSMM, M_WAITOK);
	ckptd_args->slsp = slsp;
	ckptd_args->pcaller = NULL;
	ckptd_args->recurse = args->recurse;
	ckptd_args->nextepoch = &nextepoch;

	/*
	 * If this is a one-off checkpoint, this thread will wait on the
	 * partition. This causes a deadlock when the daemon tries to force its
	 * process into single threading mode. Get into and out of single
	 * threading mode by itself and let the daemon know.
	 */
	if (slsp->slsp_attr.attr_period == 0) {
		PROC_LOCK(p);
		_PHOLD(p);
		thread_single(p, SINGLE_BOUNDARY);
		PROC_UNLOCK(p);
		ckptd_args->pcaller = p;
	}

	/* Create the daemon. */
	error = kthread_add((void (*)(void *))sls_checkpointd, ckptd_args, NULL,
	    NULL, 0, 0, "sls_checkpointd");
	if (error != 0) {
		free(ckptd_args, M_SLSMM);
		goto error;
	}

	mtx_lock(&slsp->slsp_syncmtx);

	/* If it's a periodic daemon, don't wait for it. */
	if (slsp->slsp_attr.attr_period != 0) {
		mtx_unlock(&slsp->slsp_syncmtx);
		return (error);
	}

	error = slsp_waitfor(slsp);
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	_PRELE(p);
	PROC_UNLOCK(p);

	if (error != 0)
		return (error);

	/* Give the next epoch to userspace if it asks for it. */
	if (args->nextepoch != NULL)
		error = copyout(&nextepoch, args->nextepoch, sizeof(nextepoch));

	return (error);

error:
	if (slsp->slsp_attr.attr_period == 0) {
		PROC_LOCK(p);
		thread_single_end(p, SINGLE_BOUNDARY);
		_PRELE(p);
		PROC_UNLOCK(p);
	}

	slsp_deref(slsp);

	return (error);
}

int
sls_restore(struct sls_restore_args *args)
{
	struct sls_restored_args *restd_args;
	struct slspart *slsp;
	int error;

	/* Try to get a reference to the module. */
	if (sls_startop(true) != 0)
		return (EBUSY);

	/* Get the partition to be checkpointed. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL) {
		error = EINVAL;
		goto error;
	}

	/* Make sure an in-memory checkpoint already has data. */
	if ((slsp->slsp_attr.attr_target == SLS_MEM) &&
	    (slsp->slsp_sckpt == NULL)) {
		error = EINVAL;
		goto error;
	}

	/* Set up the arguments. */
	restd_args = malloc(sizeof(*restd_args), M_SLSMM, M_WAITOK);
	restd_args->slsp = slsp;
	restd_args->daemon = args->daemon;
	restd_args->rest_stopped = args->rest_stopped;


	/* Create the daemon. */
	error = kthread_add((void (*)(void *))sls_restored, restd_args, curproc,
	    NULL, 0, 0, "sls_restored");
	if (error != 0) {
		free(restd_args, M_SLSMM);
		goto error;
	}

	mtx_lock(&slsp->slsp_syncmtx);

	return (slsp_waitfor(slsp));

error:
	if (slsp != NULL)
		slsp_deref(slsp);

	sls_finishop();

	return (error);
}

/* Add a process to an SLS partition, allowing it to be checkpointed. */
int
sls_attach(struct sls_attach_args *args)
{
	struct proc *p;
	int error;

	/* Check whether the process exists. */
	error = pget(args->pid, PGET_WANTREAD, &p);
	if (error != 0)
		return (error);

	/* Try to add the new process. */
	error = slsp_attach(args->oid, args->pid);
	if (error != 0) {
		PRELE(p);
		return (error);
	}

	SLS_LOCK();
	PROC_LOCK(p);
	sls_procadd(args->oid, p, false);
	PROC_UNLOCK(p);
	SLS_UNLOCK();

	PRELE(p);
	return (0);
}

/* Create a new, empty partition in the SLS. */
static int
sls_partadd(struct sls_partadd_args *args)
{
	struct slspart *slsp = NULL;
	int error;

	/* We are only using the SLOS for now. Later we will be able to use the
	 * network. */
	if ((args->attr.attr_target != SLS_OSD) &&
	    (args->attr.attr_target != SLS_MEM))
		return (EINVAL);

	/* Only full checkpoints make sense if in-memory. */
	if (args->attr.attr_target == SLS_MEM)
		args->attr.attr_mode = SLS_FULL;

	/*
	 * Check that the attributes to be passed
	 * to the SLS process are valid.
	 */
	if (args->attr.attr_mode >= SLS_MODES)
		return (EINVAL);

	if (args->attr.attr_target >= SLS_TARGETS)
		return (EINVAL);

	/* Check if the OID is in range. */
	if (args->oid < SLS_OIDMIN || args->oid > SLS_OIDMAX)
		return (EINVAL);

	/* Copy the SLS attributes to be given to the new process. */
	error = slsp_add(args->oid, args->attr, &slsp);
	if (error != 0)
		return (error);

	/* Specify the checkpointing strategy and the backend. */
	slsp->slsp_attr = args->attr;

	return (0);
}

/* Remove a partition from the SLS. All processes in the partition are removed.
 */
static int
sls_partdel(struct sls_partdel_args *args)
{
	struct slspart *slsp;

	/* Try to find the process. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (EINVAL);

	/* We got a reference to the process from slsp_find, free it. */
	slsp_deref(slsp);

	/*
	 * Set the status of the partition as detached, notifying
	 * processes currently checkpointing it to exit.
	 */
	slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_DETACHED, true);

	/*
	 * Check the state directly - there might be a benign race between
	 * slsp_del() instances that causes slsp_setstate() to fail.
	 */
	KASSERT(
	    slsp_getstate(slsp) == SLSP_DETACHED, ("Partition still alive"));

	/*
	 * Dereference the partition. We can't just delete it,
	 * since it might still be checkpointing.
	 */
	slsp_deref(slsp);

	return (0);
}

static int
sls_epochwait(struct sls_epochwait_args *args)
{
	struct slspart *slsp;
	int error = 0;
	bool isdone;

	/* Try to find the process. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (EINVAL);

	mtx_lock(&slsp->slsp_epochmtx);

	/* Asynchronous case, just return with an answer. */
	if (!args->sync) {
		isdone = (args->epoch <= slsp->slsp_epoch);
		error = copyout(&isdone, args->isdone, sizeof(isdone));
		goto out;
	}

	/* Synchronous case, sleep until done. */
	while (args->epoch > slsp->slsp_epoch)
		cv_wait(&slsp->slsp_epochcv, &slsp->slsp_epochmtx);

out:
	mtx_unlock(&slsp->slsp_epochmtx);

	/* Free the reference given by slsp_find(). */
	slsp_deref(slsp);
	return (error);
}

static int
sls_memsnap(struct sls_memsnap_args *args)
{
	struct proc *p = curproc;
	struct slspart *slsp;
	uint64_t nextepoch;
	int error = 0;

	/*
	 * Try to find the process. The partition is released inside the
	 * operation.
	 */
	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (EINVAL);

	PHOLD(p);
	error = slsckpt_dataregion(slsp, curproc, args->addr, &nextepoch);
	PRELE(p);

	if (error != 0)
		return (error);

	/* Give the next epoch to userspace if it asks for it. */
	if (args->nextepoch != NULL)
		error = copyout(&nextepoch, args->nextepoch, sizeof(nextepoch));

	return (error);
}

static int
sls_insls(struct sls_insls_args *args)
{
	struct proc *p = curproc;
	struct slspart *slsp;
	uint64_t oid;
	bool insls;
	int error;

	insls = false;
	oid = 0;

	/*
	 * Find the partition, if it exists. If not, the process is not in the
	 * SLS and we can return.
	 */
	slsp = slsp_find(p->p_auroid);
	if (slsp == NULL)
		goto out;

	/* Look for the process inside the partition. */
	if (slsp_hasproc(slsp, p->p_pid)) {
		insls = true;
		oid = slsp->slsp_oid;
	}

	slsp_deref(slsp);

out:
	error = copyout(&oid, args->oid, sizeof(oid));
	if (error != 0)
		return (error);

	error = copyout(&insls, args->insls, sizeof(insls));
	if (error != 0)
		return (error);

	return (0);
}

static int
sls_sysctl_init(void)
{
	struct sysctl_oid *root;

	sysctl_ctx_init(&aurora_ctx);

	root = SYSCTL_ADD_ROOT_NODE(&aurora_ctx, OID_AUTO, "aurora", CTLFLAG_RW,
	    0, "Aurora statistics and configuration variables");

	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "metadata_sent", CTLFLAG_RD, &sls_metadata_sent, 0,
	    "Bytes of metadata sent to the disk");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "metadata_received", CTLFLAG_RD, &sls_metadata_sent, 0,
	    "Bytes of metadata received from the disk");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "data_sent", CTLFLAG_RD, &sls_data_sent, 0,
	    "Bytes of data sent to the disk");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "data_received", CTLFLAG_RD, &sls_data_sent, 0,
	    "Bytes of data received from the disk");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "pages_grabbed", CTLFLAG_RD, &sls_pages_grabbed, 0,
	    "Pages grabbed by the SLS");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "io_initiated", CTLFLAG_RD, &sls_io_initiated, 0,
	    "IOs to disk initiated");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "contig_limit", CTLFLAG_RW, &sls_contig_limit, 0,
	    "Limit of contiguous IOs");
	(void)SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "drop_io", CTLFLAG_RW, &sls_drop_io, 0, "Drop all IOs immediately");
	(void)SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "vfs_sync", CTLFLAG_RW, &sls_vfs_sync, 0,
	    "Sync to the device after finishing dumping");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "ckpt_attempted", CTLFLAG_RW, &sls_ckpt_attempted, 0,
	    "Checkpoints attempted");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "ckpt_done", CTLFLAG_RW, &sls_ckpt_done, 0,
	    "Checkpoints successfully done");
	(void)SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "ckpt_duration", CTLFLAG_RW, &sls_ckpt_duration, 0,
	    "Total run time of the checkpointer");
	(void)SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "async_slos", CTLFLAG_RW, &sls_async_slos, 0,
	    "Asynchronous SLOS writes");
	(void)SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO,
	    "objprotect", CTLFLAG_RW, &sls_objprotect, 0,
	    "Traverse VM objects instead of entries when applying COW");

	return (0);
}

static int
sls_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int flag __unused,
    struct thread *td)
{
	int error = 0;

	if (sls_startop(true))
		return (EBUSY);

	switch (cmd) {
		/* Attach a process into an SLS partition. */
	case SLS_ATTACH:
		error = sls_attach((struct sls_attach_args *)data);
		break;

		/* Create an empty partition in the SLS. */
	case SLS_PARTADD:
		error = sls_partadd((struct sls_partadd_args *)data);
		break;

		/* Detach a partition from the SLS. */
	case SLS_PARTDEL:
		error = sls_partdel((struct sls_partdel_args *)data);
		break;

		/* Checkpoint a process already in the SLS. */
	case SLS_CHECKPOINT:
		error = sls_checkpoint((struct sls_checkpoint_args *)data);
		break;

		/* Restore a process from a backend. */
	case SLS_RESTORE:
		error = sls_restore((struct sls_restore_args *)data);
		break;

	case SLS_EPOCHWAIT:
		error = sls_epochwait((struct sls_epochwait_args *)data);
		break;

	case SLS_MEMSNAP:
		error = sls_memsnap((struct sls_memsnap_args *)data);
		break;

	case SLS_METROPOLIS:
		error = sls_metropolis((struct sls_metropolis_args *)data);
		break;

	case SLS_INSLS:
		error = sls_insls((struct sls_insls_args *)data);
		break;

	case SLS_METROPOLIS_SPAWN:
		error = sls_metropolis_spawn(
		    (struct sls_metropolis_spawn_args *)data);
		break;

	default:
		error = EINVAL;
		break;
	}

	sls_finishop();

	return (error);
}

static struct cdevsw slsmm_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl = sls_ioctl,
};

static int
SLSHandler(struct module *inModule, int inEvent, void *inArg)
{
	int error = 0;
	struct vnode __unused *vp = NULL;
	struct sls_partadd_args partadd_args;
	uint64_t objid;
	bitstr_t *bitmap;

	switch (inEvent) {
	case MOD_LOAD:
		bzero(&slsm, sizeof(slsm));

		mtx_init(&slsm.slsm_mtx, "slsm", NULL, MTX_DEF);
		cv_init(&slsm.slsm_exitcv, "slsm");

		error = slskv_init();
		if (error)
			return (error);

		error = slstable_init();
		if (error)
			return (error);

		/* The ckpt and restore zones depend on the kv zone. */
		error = slsckpt_zoneinit();
		if (error != 0)
			return (error);

		error = slsrest_zoneinit();
		if (error != 0)
			return (error);

		error = slskv_create(&slsm.slsm_procs);
		if (error != 0)
			return (error);

		error = slskv_create(&slsm.slsm_parts);
		if (error != 0)
			return (error);

		error = slskv_create(&slsm.slsm_prefault);
		if (error != 0)
			return (error);

		error = slskv_create(&slsm.slsm_resident);
		if (error != 0)
			return (error);

		/* Initialize Aurora-related sysctls. */
		sls_sysctl_init();

		/* Create a default on-disk and in-memory partition */
		partadd_args.oid = SLS_DEFAULT_PARTITION;
		partadd_args.attr.attr_mode = SLS_DELTA;
		partadd_args.attr.attr_target = SLS_OSD;
		partadd_args.attr.attr_period = 0;
		partadd_args.attr.attr_flags = 0;
		error = sls_partadd(&partadd_args);
		if (error) {
			printf("Problem creating default on-disk partition\n");
		}

		partadd_args.oid = SLS_DEFAULT_MPARTITION;
		partadd_args.attr.attr_mode = SLS_FULL;
		partadd_args.attr.attr_target = SLS_MEM;
		partadd_args.attr.attr_period = 0;
		partadd_args.attr.attr_flags = 0;
		error = sls_partadd(&partadd_args);
		if (error) {
			printf(
			    "Problem creating default in-memory partition\n");
		}

		/* Commandeer the swap pager. */
		sls_pager_register();

		/* Construct the system call vector. */
		slssyscall_initsysvec();
		slsmetropolis_initsysvec();
		sls_exit_hook = sls_exit_procremove;
		vm_fault_metropolis_hook = sls_register_fault;

		/* Make the SLS available to userspace. */
		slsm.slsm_cdev = make_dev(
		    &slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "sls");

		KASSERT(error == 0, ("megatable creation failed"));
		break;

	case MOD_UNLOAD:

		SLS_LOCK();
		printf("Waiting for all operations in progress...\n");
		vm_fault_metropolis_hook = NULL;
		/* Signal that we're exiting and wait for threads to finish. */
		slsm.slsm_exiting = 1;
		/* Wait for all operations to end. */
		while (slsm.slsm_inprog > 0)
			cv_wait(&slsm.slsm_exitcv, &slsm.slsm_mtx);

		printf("Killing all processes in Aurora...\n");

		/* Kill all processes in Aurora. This call can sleep. */
		error = sls_prockillall();
		if (error != 0)
			return (EBUSY);

		printf("Waiting for all processes to exit...\n");

		/* Wait for all operations to end. */
		while (!LIST_EMPTY(&slsm.slsm_plist))
			cv_wait(&slsm.slsm_exitcv, &slsm.slsm_mtx);
		SLS_UNLOCK();

		printf("Cleaning up any pending writes in the taskqueue...\n");
		/* Clean up any pending write operations. */
		slstable_fini();

		printf("Destroying all partitions...\n");
		/* Destroy all partitions. */
		slsp_delall();

		/* Destroy the prefault bitmaps. */
		if (slsm.slsm_prefault != NULL) {
			KV_FOREACH_POP(slsm.slsm_prefault, objid, bitmap)
			free(bitmap, M_SLSMM);
			slskv_destroy(slsm.slsm_prefault);
		}

		/* Destroy the resident page bitmaps . */
		if (slsm.slsm_resident != NULL) {
			KV_FOREACH_POP(slsm.slsm_resident, objid, bitmap)
			free(bitmap, M_SLSMM);
			slskv_destroy(slsm.slsm_resident);
		}

		/*
		 * Swap off the Aurora swapper.  XXX Ideally we'd like to kill
		 * every single process that could conceivably use Aurora swap
		 * objects; this includes processes created from restored Aurora
		 * processes using fork(), and processes that have been orphaned
		 * and given to init(). If runnin a partition in a container, as
		 * we should, this is easy; we traverse the process tree of the
		 * container init process. If not, we need to override the
		 * system call vectors of all processes in Aurora to add
		 * themselves to the process list.
		 */
		SLS_LOCK();
		printf("Destroying the swapper...\n");
		sls_pager_swapoff();

		printf("Waiting for all swap objects to die...\n");
		while (slsm.slsm_swapobjs > 0)
			cv_wait(&slsm.slsm_exitcv, &slsm.slsm_mtx);

		SLS_UNLOCK();

		sls_pager_unregister();

		/* We need to have powered Aurora off. */
		if (slsm.slsm_exiting == 0)
			return (EINVAL);

		sls_exit_hook = NULL;
		slsmetropolis_finisysvec();
		slssyscall_finisysvec();

		/*
		 * Destroy the device, wait for all ioctls in progress. We do
		 * this without the non-sleepable module lock.
		 */
		if (slsm.slsm_cdev != NULL)
			destroy_dev(slsm.slsm_cdev);

		if (sysctl_ctx_free(&aurora_ctx))
			printf("Failed to destroy sysctl\n");

		slsrest_zonefini();
		slsckpt_zonefini();
		slskv_fini();

		cv_destroy(&slsm.slsm_exitcv);
		mtx_destroy(&slsm.slsm_mtx);
		printf("Done.\n");

		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static moduledata_t moduleData = { "sls", SLSHandler, NULL };

DECLARE_MODULE(sls, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_DEPEND(sls, slsfs, 0, 0, 0);
MODULE_VERSION(sls, 0);
