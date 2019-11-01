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

/* Launch a checkpointing daemon. */
static int
sls_start_checkpointing(struct proc *p, struct slspart *slsp)
{
	struct sls_checkpointd_args *ckptd_args;
	int error = 0;

	/* Set up the arguments. */
	ckptd_args = malloc(sizeof(*ckptd_args), M_SLSMM, M_WAITOK);
	ckptd_args->slsp = slsp;
	ckptd_args->p = p;

	/* Create the daemon. */
	error = kthread_add((void(*)(void *)) sls_checkpointd, 
	    ckptd_args, NULL, NULL, 0, 0, "sls_checkpointd");
	if (error != 0)
	    free(ckptd_args, M_SLSMM);

	return error;
}

/* Request a one-off checkpoint. */
static int
sls_checkpoint(struct sls_checkpoint_args *args)
{
	struct slspart *slsp;
	struct proc *p = NULL;
	int error = 0;

	/* Get the in-SLS process to be checkpointed. */
	slsp = slsp_find(args->pid);
	if (slsp == NULL)
	    return EINVAL;

	/*
	 * Get a hold of the process to be checkpointed, bringing its 
	 * thread stack to memory and preventing it from exiting.
	 */
	error = pget(args->pid, PGET_WANTREAD, &p);
	if (error != 0) {
	    /* 
	     * If we could not get a hold of the process, that means
	     * that it has probably exited. Remove it from the SLS.
	     */
	    slsp_deref(slsp);
	    goto error;
	}

	/* Launch a checkpointd thread for the process. */
	error = sls_start_checkpointing(p, slsp);
	if (error != 0)
	    goto error;

	return 0;

error:

	if (p != NULL)
	    PRELE(p);

	/* If we got a reference to the process in the SLS, free it. */
	if (slsp != NULL)
	    slsp_deref(slsp);

	return error;
}


static int
sls_restore(struct sls_restore_args *args)
{
	struct sls_restored_args *restd_args = NULL;
	struct proc *p = NULL;
	struct sbuf *filename = NULL;
	struct sls_backend backend; 
	int error = 0;

	/* Check if the backend type being requested is valid. */
	if (args->backend.bak_target >= SLS_TARGETS) {
	    printf("Error: Invalid restore from backend %d\n", args->backend.bak_target);
	    return EINVAL;
	}

	/*
	 * Get a hold of the process to be checkpointed, bringing its 
	 * thread stack to memory and preventing it from exiting.
	 */
	error = pget(args->pid, PGET_WANTREAD, &p);
	if (error != 0) {
	    printf("Error: pget failed with %d\n", error);
	    return error;
	}

	/* If the source is a file, bring its name into the kernel. */
	switch (args->backend.bak_target) {
	case SLS_FILE:
	    /* XXX Temporarily turn off the file backend. */
	    error = EINVAL;
	    goto error;

#if 0
	    /* Get a new sbuf to read the filename into. */
	    filename = sbuf_new_auto();
	    if (filename == NULL) {
		error = ENOMEM;
		goto error;
	    }

	    /* Read the filename from userspace. */
	    len = sbuf_copyin(filename, args->data, PATH_MAX);
	    if (len < 0) {
		error = EACCES;
		goto error;
	    }

	    /* Finalize the sbuf. */
	    sbuf_finish(filename);

	    /* Construct the kernel-side backend. */
	    backend.bak_target = SLS_FILE;
	    backend.bak_name = filename;
	    break;
#endif

	case SLS_OSD:

	    /* There are no pointers in the backend when it's for the OSD. */
	    backend = args->backend;

	    break;

	default:
	    error = EINVAL;
	    goto error;
	}
	    

	/* Set up the arguments for the restore. */
	restd_args = malloc(sizeof(*restd_args), M_SLSMM, M_WAITOK);
	restd_args->p = p;
	restd_args->backend = backend;

	/* 
	 * While for sls_checkpoint we can assign the worker
	 * thread to init, for restore we need to have the
	 * thread in the process on top of which we restore.
	 */
	error = kthread_add((void(*)(void *)) sls_restored, 
	    restd_args, p, NULL, 0, 0, "sls_restored");
	if (error != 0)
	    goto error;

	/* Destroy this thread, it's not needed anymore. */
	kern_thr_exit(curthread);

	return error;

error:

	free(restd_args, M_SLSMM);

	if (filename != NULL)
	    sbuf_delete(filename);

	if (p != NULL)
	    PRELE(p);

	return error;
}


/* Add a process to the SLS, allowing it to be checkpointed. */
static int
sls_attach(struct sls_attach_args *args)
{
	struct slspart *slsp = NULL;
	struct proc *p = NULL;
	struct sbuf *filename = NULL;
	int error;
	size_t len;

	/* XXX We do not have the capability to dump to memory yet. */
	if (args->attr.attr_backend.bak_target == SLS_MEM)
	    return EINVAL;

	/* 
	 * Check that the attributes to be passed
	 * to the SLS process are valid.
	 */
	if (args->attr.attr_mode >= SLS_MODES)
	    return EINVAL;

	if (args->attr.attr_backend.bak_target >= SLS_TARGETS)
	    return EINVAL;

	/*
	 * Actually check whether the process exists.
	 */
	error = pget(args->pid, PGET_WANTREAD, &p);
	if (error != 0)
	    return error;

	/* Try to add the new process. */
	slsp = slsp_add(args->pid);
	if (slsp == NULL)
	    return EINVAL;

	/* 
	 * Copy the SLS attributes to be given to the new process. We only need
	 * to deep copy if we are using a file to checkpoint, in which case 
	 * we need to bring in the sbuf in kernel memory.
	 */
	slsp->slsp_attr = args->attr;

	if (slsp->slsp_attr.attr_backend.bak_target == SLS_FILE) {
	    /* 
	     * We normally hold the filename in an sbuf. However, we
	     * can't copy an sbuf from userspace to the kernel as-is,
	     * and must instead use the raw pointer. We use the data
	     * field of the argument for that. 
	     */
	    filename = sbuf_new_auto();
	    len = sbuf_copyin(filename, args->data, PATH_MAX);
	    if (len < 0) {
		error = EACCES;
		goto error;
	    }

	    /* Finalize the sbuf. */
	    sbuf_finish(filename);
	    slsp->slsp_attr.attr_backend.bak_name = filename;
	}

	/* 
	 * If the checkpointing period specified is nonzero, 
	 * we start checkpointing immediately.
	 */
	if (slsp->slsp_attr.attr_period != 0) {
	    /* Get another reference for the in-SLS process. */
	    slsp_ref(slsp);
		
	    error = sls_start_checkpointing(p, slsp);
	    if (error != 0)
		goto error;
	} else {
	    /* Otherwise we don't to hold the process anymore. */
	    PRELE(p);

	}

	return 0;

error:

	if (filename != NULL)
	    sbuf_delete(filename);

	if (p != NULL)
	    PRELE(p);

	if (slsp != NULL)
	    slsp_deref(slsp);

	return error;
}

/* Remove a process from the SLS. */
static int
sls_detach(struct sls_detach_args *args)
{
	struct slspart *slsp;
	
	/* Try to find the process. */
	slsp = slsp_find(args->pid);
	if (slsp == NULL)
	    return EINVAL;

	/* We got a reference to the process from slsp_find, free it. */
	slsp_deref(slsp);

	/* 
	 * Set the status of the process as detached, notifying
	 * processes currently checkpointing it to exit.
	 */
	atomic_set_int(&slsp->slsp_status, SPROC_DETACHED);

	/* 
	 * Dereference the process. We can't just delete it,
	 * since it might still be under checkpointing.
	 */
	slsp_deref(slsp);

	return 0;
}

static int
sls_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
	int flag __unused, struct thread *td)
{
	struct proc_param *pparam = NULL;
	struct slspart *slsp;
	int error = 0;

	switch (cmd) {
	    /* Attach a process into the SLS. */
	    case SLS_ATTACH:
		error = sls_attach((struct sls_attach_args *) data);
		break;

	    /* Detach a process from the SLS. */
	    case SLS_DETACH:
		error = sls_detach((struct sls_detach_args *) data);
		break;

	    /* Checkpoint a process already in the SLS. */
	    case SLS_CHECKPOINT:
		error = sls_checkpoint((struct sls_checkpoint_args *) data);
		break;

	    /* Restore a process from a backend. */
	    case SLS_RESTORE:
		error = sls_restore((struct sls_restore_args *) data);
		break;

	    /* XXX Refactor */
	    case SLS_PROCSTAT:
		pparam = (struct proc_param *) data;

		/* Try to find the process. */
		slsp = slsp_find(pparam->pid);
		if (slsp == NULL)
		    return EINVAL;

		/* Copy out the status of the process. */
		error = copyout(&slsp->slsp_status, pparam->ret, sizeof(*pparam->ret));

		/* Free the reference given by slsp_find(). */
		slsp_deref(slsp);
		return error;

	    default:
		return EINVAL;
	}

	return error;
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

	    slskv_zone = uma_zcreate("SLS table pairs", 
		    sizeof(struct slskv_pair), NULL, NULL, NULL, 
		    NULL, UMA_ALIGNOF(struct slskv_pair), 0);
	    if (slskv_zone == NULL) {
		error = ENOMEM;
		break;
	    }

	    slspagerun_zone = uma_zcreate("SLS pageruns", 
		    sizeof(struct slspagerun), NULL, NULL, NULL, 
		    NULL, UMA_ALIGNOF(struct slspagerun), 0);
	    if (slspagerun_zone == NULL) {
		error = ENOMEM;
		break;
	    }

	    error = slskv_create(&slsm.slsm_proctable, SLSKV_NOREPLACE);
	    if (error != 0)
		return error;

	    error = slskv_create(&slsm.slsm_rectable, SLSKV_NOREPLACE);
	    if (error != 0)
		return error;

	    error = slskv_create(&slsm.slsm_typetable, SLSKV_NOREPLACE);
	    if (error != 0)
		return error;

	    slsm.slsm_cdev = 
		make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "sls");

#ifdef SLS_TEST 
	    printf("Testing slstable component...\n");
	    error = slstable_test();
	    if (error != 0)
		return error;

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

	    if (slsm.slsm_typetable != NULL)
		slskv_destroy(slsm.slsm_typetable);

	    if (slsm.slsm_rectable != NULL)
		slskv_destroy(slsm.slsm_rectable);

	    if (slsm.slsm_proctable != NULL)
		slskv_destroy(slsm.slsm_proctable);

	    if (slspagerun_zone != NULL)
		uma_zdestroy(slspagerun_zone);

	    if (slskv_zone!= NULL)
		uma_zdestroy(slskv_zone);

	    printf("SLS Unloaded.\n");
	    break;
	default:
	    error = EOPNOTSUPP;
	    break;
    }
    return error;
}



static moduledata_t moduleData = {
    "sls",
    SLSHandler,
    NULL
};

DECLARE_MODULE(sls, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_DEPEND(sls, slos, 0, 0, 0);
MODULE_VERSION(sls, 0);
