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

MALLOC_DEFINE(M_SLSMM, "slsmm", "SLSMM");

struct sls_metadata slsm;

/* 
 * Start checkpointing a partition. If a checkpointing period
 * has been set, then the partition gets periodically 
 * checkpointed, otherwise it's a one-off.
 */
static int
sls_checkpoint(struct sls_checkpoint_args *args)
{
	struct sls_checkpointd_args *ckptd_args;
	struct slspart *slsp;
	int error = 0;

	/* Get the partition to be checkpointed. */
	slsp = slsp_find(args->oid);
	if (slsp == NULL)
	    return (EINVAL);

	/* Set up the arguments. */
	ckptd_args = malloc(sizeof(*ckptd_args), M_SLSMM, M_WAITOK);
	ckptd_args->slsp = slsp;
	ckptd_args->recurse = args->recurse;

	/* Create the daemon. */
	error = kthread_add((void(*)(void *)) sls_checkpointd, 
	    ckptd_args, NULL, NULL, 0, 0, "sls_checkpointd");
	if (error != 0) {
	    free(ckptd_args, M_SLSMM);
	    slsp_deref(slsp);
	}

	return (0);
}

static int
sls_restore(struct sls_restore_args *args)
{
	struct sls_restored_args *restd_args = NULL;

	/* Set up the arguments for the restore. */
	restd_args = malloc(sizeof(*restd_args), M_SLSMM, M_WAITOK);
	restd_args->oid = args->oid;
	restd_args->daemon = args->daemon;

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
	atomic_set_int(&slsp->slsp_status, SPROC_DETACHED);

	/* 
	 * Dereference the partition. We can't just delete it,
	 * since it might still be checkpointing.
	 */
	slsp_deref(slsp);

	return (0);
}

static int
sls_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
	int flag __unused, struct thread *td)
{
	struct proc_param *pparam = NULL;
	struct slspart *slsp;
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

	    /* XXX This is a leftover from forever ago, remove. */
	    case SLS_PROCSTAT:
		pparam = (struct proc_param *) data;

		/* Try to find the process. */
		slsp = slsp_find(pparam->pid);
		if (slsp == NULL)
		    return (EINVAL);

		/* Copy out the status of the process. */
		error = copyout(&slsp->slsp_status, pparam->ret, sizeof(*pparam->ret));

		/* Free the reference given by slsp_find(). */
		slsp_deref(slsp);
		return (error);

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
    
    switch (inEvent) {
	case MOD_LOAD:
	    bzero(&slsm, sizeof(slsm));

	    mtx_init(&slsm.slsm_mtx, "SLS main mutex", NULL, MTX_DEF);
	    cv_init(&slsm.slsm_proccv, "SLS process restore lock");
	    cv_init(&slsm.slsm_donecv, "SLS general restore lock");

	    error = slskv_init();
	    if (error)
		return (error);

	    slspagerun_zone = uma_zcreate("SLS pageruns", 
		    sizeof(struct slspagerun), NULL, NULL, NULL, 
		    NULL, UMA_ALIGNOF(struct slspagerun), 0);
	    if (slspagerun_zone == NULL) {
		error = ENOMEM;
		break;
	    }

	    error = slskv_create(&slsm.slsm_procs);
	    if (error != 0)
		return (error);

	    error = slskv_create(&slsm.slsm_parts);
	    if (error != 0)
		return (error);

	    slsm.slsm_cdev = 
		make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "sls");

#ifdef SLS_TEST 
	    printf("Testing slstable component...\n");
	    error = slstable_test();
	    if (error != 0)
		return (error);

#endif /* SLS_TEST */
	    printf("SLS Loaded.\n");
	    break;

	/*
	 * XXX Don't export OSD right now, we
	 * don't care since we're going to create
	 * proper mount/unmount code anyway and
	 * pull it out of here.
	 */
	case MOD_UNLOAD:

	    slsm.slsm_exiting = 1;

	    if (slsm.slsm_cdev != NULL)
		destroy_dev(slsm.slsm_cdev);

	    slsp_delall();

	    if (slsm.slsm_parts != NULL)
		slskv_destroy(slsm.slsm_parts);

	    slskv_fini();

	    cv_destroy(&slsm.slsm_donecv);
	    cv_destroy(&slsm.slsm_proccv);
	    mtx_destroy(&slsm.slsm_mtx);

	    printf("SLS Unloaded.\n");
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
