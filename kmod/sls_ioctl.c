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
#include <sys/mutex.h>
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include "sls_internal.h"
#include "sls_ioctl.h"
#include "sls_kv.h"
#include "sls_mm.h"
#include "sls_table.h"

/* XXX Rename to M_SLS. */
MALLOC_DEFINE(M_SLSMM, "sls", "SLS");
MALLOC_DEFINE(M_SLSREC, "slsrec", "SLSREC");

extern int sls_objprotect;

struct sls_metadata slsm;
struct sysctl_ctx_list aurora_ctx;

/*
 * This mutex/condition variable would need more work to be turned into per
 * process entities, so we just dump them in here and sacrifice parallelism when
 * restoring multiple partitions at the same time (NOTE: partitions, not
 * processes. Multiple processes in the same partition are fine.)
 */
struct mtx sls_restmtx;
struct cv sls_restcv;
int sls_resttds = -1;

/*
 * Start checkpointing a partition. If a checkpointing period
 * has been set, then the partition gets periodically
 * checkpointed, otherwise it's a one-off.
 */
static int
sls_checkpoint(struct sls_checkpoint_args *args)
{
	struct sls_checkpointd_args *ckptd_args;
	struct proc *p = curproc;
	struct slspart *slsp;
	int error = 0;

	/* Try to get a reference to the module. */
	if (sls_modref() != 0)
		return (EINVAL);

	/* Take another reference for the worker thread. */
	if (sls_modref() != 0) {
		sls_modderef();
		return (EINVAL);
	}

	/* Get the partition to be checkpointed. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		goto error;

	/* Cannot do periodic checkpoints synchronously. */
	if ((slsp->slsp_attr.attr_period != 0) &&
		(args->synchronous != 0)) {
		slsp_deref(slsp);
		goto error;
	}

	/* Set up the arguments. */
	ckptd_args = malloc(sizeof(*ckptd_args), M_SLSMM, M_WAITOK);
	ckptd_args->slsp = slsp;
	ckptd_args->recurse = args->recurse;
	ckptd_args->sync = args->synchronous;
	if (args->synchronous) {
		mtx_lock(&slsp->slsp_syncmtx);

		/*
		 * We cannot induce single threading for this process if we are
		 * waiting on the CV below. Turn this process into single
		 * threading mode here.
		 */
		PROC_LOCK(p);
		thread_single(p, SINGLE_BOUNDARY);
		PROC_UNLOCK(p);
	}

	/* Create the daemon. */
	error = kthread_add((void(*)(void *)) sls_checkpointd,
	    ckptd_args, NULL, NULL, 0, 0, "sls_checkpointd");
	if (error != 0) {
		if (args->synchronous)
			mtx_unlock(&slsp->slsp_syncmtx);

		free(ckptd_args, M_SLSMM);
		slsp_deref(slsp);
		goto error;
	}

	if (args->synchronous) {
		slsp_waitfor(slsp);

		/*
		 * No need to get ourselves out of single threading mode,
		 * the daemon already did it for us.
		 */
	}

	sls_modderef();
	return (0);

error:

	/* Release both references from here. */
	sls_modderef();
	sls_modderef();

	return (EINVAL);
}

static int
sls_restore(struct sls_restore_args *args)
{
	struct sls_restored_args *restd_args = NULL;

	/* Try to get a reference to the module. */
	if (sls_modref() != 0)
		return (EINVAL);

	/* Set up the arguments for the restore. */
	restd_args = malloc(sizeof(*restd_args), M_SLSMM, M_WAITOK);
	restd_args->oid = args->oid;
	restd_args->daemon = args->daemon;
	restd_args->rest_stopped = args->rest_stopped;

	/* 
	 * The sls_restored function can alternatively
	 * be called as a separate kernel process, but
	 * this poses its own set of problems, so we
	 * use it directly. 
	 */
	sls_restored(restd_args);

	return (0);
}

/* Add a process to an SLS partition, allowing it to be checkpointed. */
static int
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

	PRELE(p);
	return (0);
}

/* Create a new, empty partition in the SLS. */
static int
sls_partadd(struct sls_partadd_args *args)
{
	struct slspart *slsp = NULL;
	int error;

	/* We are only using the SLOS for now. Later we will be able to use the network. */
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

/* Remove a partition from the SLS. All processes in the partition are removed. */
static int
sls_partdel(struct sls_partdel_args *args)
{
	struct slspart *slsp;

	/* XXX Use partition-local locks. */

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
	atomic_set_int(&slsp->slsp_status, SLSPART_DETACHED);

	/*
	 * Dereference the partition. We can't just delete it,
	 * since it might still be checkpointing.
	 */
	slsp_deref(slsp);

	return (0);
}

static int
sls_epoch(struct sls_epoch_args *args)
{
	struct slspart *slsp;
	int error;

	/* Try to find the process. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (EINVAL);

	/* Copy out the status of the process. */
	error = copyout(&slsp->slsp_epoch, args->ret,
		sizeof(slsp->slsp_epoch));

	/* Free the reference given by slsp_find(). */
	slsp_deref(slsp);
	return (error);

}

static int
sls_sysctl_init(void)
{
	struct sysctl_oid *root;

	sysctl_ctx_init(&aurora_ctx);

	root = SYSCTL_ADD_ROOT_NODE(&aurora_ctx, OID_AUTO, "aurora", CTLFLAG_RW, 0,
	    "Aurora statistics and configuration variables");

	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "metadata_sent",
	    CTLFLAG_RD, &sls_metadata_sent,
	    0, "Bytes of metadata sent to the disk");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "metadata_received",
	    CTLFLAG_RD, &sls_metadata_sent,
	    0, "Bytes of metadata received from the disk");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "data_sent",
	    CTLFLAG_RD, &sls_data_sent,
	    0, "Bytes of data sent to the disk");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "data_received",
	    CTLFLAG_RD, &sls_data_sent,
	    0, "Bytes of data received from the disk");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "pages_grabbed",
	    CTLFLAG_RD, &sls_pages_grabbed,
	    0, "Pages grabbed by the SLS");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "io_initiated",
	    CTLFLAG_RD, &sls_io_initiated,
	    0, "IOs to disk initiated");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "contig_limit",
	    CTLFLAG_RW, &sls_contig_limit,
	    0, "Limit of contiguous IOs");
	(void) SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "use_nulldev",
	    CTLFLAG_RW, &sls_use_nulldev,
	    0, "Route IOs to the null device");
	(void) SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "drop_io",
	    CTLFLAG_RW, &sls_drop_io,
	    0, "Drop all IOs immediately");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "iochain_size",
	    CTLFLAG_RW, &sls_iochain_size,
	    0, "Maximum IO chain size");
	(void) SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "sync",
	    CTLFLAG_RW, &sls_sync,
	    0, "Sync to the device after finishing dumping");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "ckpt_attempted",
	    CTLFLAG_RW, &sls_ckpt_attempted,
	    0, "Checkpoints attempted");
	(void) SYSCTL_ADD_U64(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "ckpt_done",
	    CTLFLAG_RW, &sls_ckpt_done,
	    0, "Checkpoints successfully done");
	(void) SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "async_slos",
	    CTLFLAG_RW, &sls_async_slos,
	    0, "Asynchronous SLOS writes");
	(void) SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "use_slos",
	    CTLFLAG_RW, &sls_use_slos,
	    0, "Use the SLOS as a backend");
	(void) SYSCTL_ADD_UINT(&aurora_ctx, SYSCTL_CHILDREN(root), OID_AUTO, "objprotect",
	    CTLFLAG_RW, &sls_objprotect,
	    0, "Traverse VM objects instead of entries when applying COW");

	return (0);
}

static int
sls_setup_blackholefp(void)
{
	int sls_blackholefd;
	int error;

	error = kern_openat(curthread, AT_FDCWD, "/dev/null", UIO_SYSSPACE, O_RDWR, 0);
	if (error != 0)
		return (error);

	sls_blackholefd = curthread->td_retval[0];
	sls_blackholefp = FDTOFP(curthread->td_proc, sls_blackholefd);
	(void) fhold(sls_blackholefp);

	error = kern_close(curthread, sls_blackholefd);
	if (error != 0)
		SLS_DBG("Could not close sls_blackholefd, error %d\n", error);

	return (error);
}

static int
sls_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
    int flag __unused, struct thread *td)
{
	int error = 0;

	switch (cmd) {
		/* Attach a process into an SLS partition. */
	case SLS_ATTACH:
		error = sls_attach((struct sls_attach_args *) data);
		break;

		/* Create an empty partition in the SLS. */
	case SLS_PARTADD:
		error = sls_partadd((struct sls_partadd_args *) data);
		break;

		/* Detach a partition from the SLS. */
	case SLS_PARTDEL:
		error = sls_partdel((struct sls_partdel_args *) data);
		break;

		/* Checkpoint a process already in the SLS. */
	case SLS_CHECKPOINT:
		error = sls_checkpoint((struct sls_checkpoint_args *) data);
		break;

		/* Restore a process from a backend. */
	case SLS_RESTORE:
		error = sls_restore((struct sls_restore_args *) data);
		break;

	case SLS_EPOCH:
		error = sls_epoch((struct sls_epoch_args *) data);
		break;

	default:
		return (EINVAL);
	}

	return (error);
}



static struct cdevsw slsmm_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl = sls_ioctl,
};

static int
SLSHandler(struct module *inModule, int inEvent, void *inArg) {
	int error = 0;
	struct vnode __unused *vp = NULL;
	struct sls_partadd_args partadd_args;

	switch (inEvent) {
	case MOD_LOAD:
		mtx_init(&sls_restmtx, "slsrest", NULL, MTX_DEF);
		cv_init(&sls_restcv, "slsrest");

		bzero(&slsm, sizeof(slsm));

		error = sls_setup_blackholefp();
		if (error != 0)
			return (error);

		mtx_init(&slsm.slsm_mtx, "slsm", NULL, MTX_DEF);
		cv_init(&slsm.slsm_exitcv, "slsm");

		error = slskv_init();
		if (error)
			return (error);

		slspagerun_zone = uma_zcreate("SLS pageruns",
		    sizeof(struct slspagerun), NULL, NULL, NULL,
		    NULL, UMA_ALIGNOF(struct slspagerun), 0);
		if (slspagerun_zone == NULL) {
			error = ENOMEM;
			return (error);
		}

		error = slskv_create(&slsm.slsm_procs);
		if (error != 0)
			return (error);

		error = slskv_create(&slsm.slsm_parts);
		if (error != 0)
			return (error);

		/* Initialize Aurora-related sysctls. */
		sls_sysctl_init();

		/* Create a default on-disk and in-memory partition */
		partadd_args.oid = SLS_DEFAULT_PARTITION;
		partadd_args.attr.attr_mode = SLS_DELTA;
		partadd_args.attr.attr_target = SLS_OSD;
		partadd_args.attr.attr_period = 1000;
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
			printf("Problem creating default in-memory partition\n");
		}

		slsm.slsm_cdev = make_dev(&slsmm_cdevsw, 0,
		    UID_ROOT, GID_WHEEL, 0666, "sls");

#ifdef SLS_TEST
		printf("Testing slstable component...\n");
		error = slstable_test();
		if (error != 0)
			return (error);

#endif /* SLS_TEST */

		break;

	case MOD_UNLOAD:

		/* Signal that we're exiting and wait for threads to finish. */
		mtx_lock(&slsm.slsm_mtx);
		slsm.slsm_exiting = 1;
		while (slsm.slsm_users > 0)
			cv_wait(&slsm.slsm_exitcv, &slsm.slsm_mtx);
		mtx_unlock(&slsm.slsm_mtx);

		/* XXX Destroy all partitions */

		if (slsm.slsm_cdev != NULL)
			destroy_dev(slsm.slsm_cdev);

		if (sysctl_ctx_free(&aurora_ctx))
			printf("Failed to destroy sysctl\n");

		slsp_delall();

		if (slsm.slsm_parts != NULL)
			slskv_destroy(slsm.slsm_parts);

		slskv_fini();

		cv_destroy(&slsm.slsm_exitcv);
		mtx_destroy(&slsm.slsm_mtx);

		if (sls_blackholefp != NULL)
			fdrop(sls_blackholefp, curthread);

		cv_destroy(&sls_restcv);
		mtx_destroy(&sls_restmtx);

		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}



static moduledata_t moduleData = {
	"sls",
	SLSHandler,
	NULL
};

DECLARE_MODULE(sls, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_DEPEND(sls, slsfs, 0, 0, 0);
MODULE_VERSION(sls, 0);
