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
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <machine/param.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "slsmm.h"
#include "sls_op.h"
#include "sls_ioctl.h"
#include "sls_snapshot.h"
#include "sls_mosd.h"
#include "path.h"

#include "sls_dump.h"


MALLOC_DEFINE(M_SLSMM, "slsmm", "SLSMM");

struct sls_metadata slsm;

static struct sls_snapshot * 
slss_from_file(char *filename)
{
    struct sls_snapshot *slss;
    int error;
    int fd;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR | O_CREAT, S_IRWXU);
    fd = curthread->td_retval[0];
    if (error != 0) {
	printf("Error: Opening file failed with %d\n", error);
	return NULL;
    }

    slss = load_dump(fd);
    kern_close(curthread, fd);

    return slss;
}


static int
slsmm_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
	int flag __unused, struct thread *td)
{
	struct op_param *oparam = NULL;
	struct compose_param *cparam = NULL;
	struct snap_param *sparam = NULL;
	struct proc_param *pparam = NULL;

	struct sls_op_args *op_args;
	struct sls_snapshot *slss;
	struct sls_process *slsp;
	struct proc *p = NULL;

	char *snap_name = NULL;
	size_t snap_name_len;

	int *ids = NULL;
	int mode;
	int error = 0;
	int target;
	int op;

	switch (cmd) {
	    case SLS_OP:

		oparam = (struct op_param *) data;

		target = oparam->target;
		op = oparam->op;

		if (op != SLS_CHECKPOINT && oparam->mode != SLS_FULL) {
		    printf("Error: Dump mode flag only valid with checkpointing\n");
		    return EINVAL;
		}

		if (target != SLS_MEM && target != SLS_FILE && target != SLS_OSD) {
		    printf("Error: Invalid checkpoint fd type %d\n", target);
		    return EINVAL;
		}

		/*
		* Get a hold of the process to be
		* checkpointed, bringing its thread stack to memory
		* and preventing it from exiting, to avoid race
		* conditions.
		*/
		/* XXX Here's where we modify for when we use in-memory checkpoints */
		error = pget(oparam->pid, PGET_WANTREAD, &p);
		if (error != 0) {
		    printf("Error: pget failed with %d\n", error);
		    return error;
		}

		/*
		 * If we are stopping a checkpoint, here's where we're done.
		 */
		/*
		if (op == SLS_CKPT_STOP)
		    return sls_checkpoint_stop(p);
		    */

		/* Use kernel-internal constants for checkpointing mode */
		if (oparam->mode == SLS_FULL)
		    mode = SLS_CKPT_FULL;
		else 
		    mode = SLS_CKPT_DELTA;


		if (target == SLS_FILE) {
		    snap_name_len = MIN(oparam->len, PATH_MAX);
		    snap_name = malloc(snap_name_len + 1, M_SLSMM, M_WAITOK);

		    error = copyin(oparam->name, snap_name, snap_name_len);
		    if (error != 0) {
			printf("Error: Copying name failed with %d\n", error);
			PRELE(p);
			return error;
		    }
		    snap_name[snap_name_len] = '\0';
		} 
		    
		op_args = malloc(sizeof(*op_args), M_SLSMM, M_WAITOK);

		op_args->filename = snap_name;
		op_args->p = p;
		op_args->target = oparam->target;
		op_args->mode = mode;
		op_args->iterations = oparam->iterations;
		op_args->interval = oparam->period;

		if (target == SLS_MEM)
		    op_args->id = oparam->id;

		if (op == SLS_CHECKPOINT) {
		    error = kthread_add((void(*)(void *)) sls_ckptd, 
			op_args, NULL, NULL, 0, 0, "sls_checkpointd");

		    return error;
		} 
		

		KASSERT(op == SLS_RESTORE, "operation is restore");
		/* 
		 * Eager check for the snapshot, because if we fail
		 * during restoring we have already trashed this process
		 * beyond repair.
		 */
		if (op_args->target == SLS_FILE)
		    slss = slss_from_file(op_args->filename);
		else
		    slss = slss_find(op_args->id);

		if (slss == NULL) {
		    PRELE(op_args->p);
		    free(op_args->filename, M_SLSMM);
		    free(op_args, M_SLSMM);
		    return EINVAL;
		}

		op_args->slss = slss;

		error = kthread_add((void(*)(void *)) sls_restd, 
		    op_args, p, NULL, 0, 0, "sls_restored");

		kern_thr_exit(curthread);

		return error;


	    case SLS_COMPOSE:
		cparam = (struct compose_param *) data;

		ids = malloc(sizeof(*ids) * cparam->nid, M_SLSMM, M_WAITOK);
		error = copyin(cparam->id, ids, sizeof(*ids) * cparam->nid);
		if (error != 0) {
		    printf("Error: Copying fds failed with %d \n", error);
		    free(ids, M_SLSMM);
		    return error;
		}

		/* 
		 * XXX Merge page hash tables between snapshots, 
		 * keep the oldest one. Free the rest.
		 */

		printf("Warning: Not implemented yet, is a no-op\n");

		free(ids, M_SLSMM);

		return error;

	    case SLS_SNAP:
		sparam = (struct snap_param *) data;

		switch (sparam->op) {
		case SLS_SNAPLIST:
		    slss_listall();
		    return 0;

		case SLS_SNAPDEL:
		    slss_delete(sparam->id);
		    return 0;

		default:
		    return EINVAL;
		}

	    case SLS_PROC:
		pparam = (struct proc_param *) data;

		/* XXX What happens if it's invalid? */
		slsp = slsp_add(pparam->pid);

		switch (pparam->op) {
		case SLS_PROCSTAT:
		    error = copyout(&slsp->slsp_active, pparam->ret, sizeof(*pparam->ret));
		    return error;

		case SLS_PROCSTOP:
		    mtx_lock(&slsp->slsp_mtx);
		    slsp->slsp_active = 0;
		    mtx_unlock(&slsp->slsp_mtx);
		    return 0;

		}
		
	}

	return EINVAL;
}


/*
 * XXX To be refactored later when benchmarking
 */
static int
log_results(int lognum)
{
	int error;
	int logfd;
	char *buf;
	int i, j;
	struct uio auio;
	struct iovec aiov;

	buf = malloc(SLS_LOG_BUFFER * sizeof(*buf), M_SLSMM, M_NOWAIT);
	if (buf == NULL)
	    return ENOMEM;

	error = kern_openat(curthread, AT_FDCWD, "/tmp/sls_benchmark", 
			    UIO_SYSSPACE, 
			    O_RDWR | O_CREAT | O_TRUNC, 
			    S_IRWXU | S_IRWXG | S_IRWXO);
	if (error != 0) {
	    printf("Could not log results, error %d", error);
	    free(buf, M_SLSMM);
	    return error;
	}

	logfd = curthread->td_retval[0];

	for (i = 0; i < lognum; i++) {
	    for (j = 0; j < 9; j++) {

		snprintf(buf, 1024, "%u,%lu\n", j, slsm.slsm_log[j][i]);

		aiov.iov_base = buf;
		aiov.iov_len = strnlen(buf, 1024);
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_resid = strnlen(buf, 1024);
		auio.uio_segflg = UIO_SYSSPACE;

		error = kern_writev(curthread, logfd, &auio);
		if (error != 0)
		    printf("Writing to file failed with %d\n", error);
	    }

	}

	kern_close(curthread, logfd);
	free(buf, M_SLSMM);

	return 0;
}

static struct cdevsw slsmm_cdevsw = {
    .d_version = D_VERSION,
    .d_ioctl = slsmm_ioctl,
};

static int
SLSHandler(struct module *inModule, int inEvent, void *inArg) {
    int error = 0;
    struct vnode *vp = NULL;
    
    switch (inEvent) {
	case MOD_LOAD:
	    bzero(&slsm, sizeof(slsm));


	    sls_osdhack();
	    /* TEMP */
	    /*
	    error = filename_to_vnode("/root/nvd0", &vp);
	    if (error != 0) {
		printf("Error: Could not find OSD\n");
		return error;
	    }

	    slsm.slsm_osdvp = vp;
	    printf("Pointer is %p\n", vp);
	    */
	    slsm.slsm_osd = slsosd_import(vp);
	    if (slsm.slsm_osd == NULL) {
		bzero(&slsm, sizeof(slsm));
		printf("Loading OSD failed\n");
	    } else {

		slsm.slsm_mbmp = mbmp_import(slsm.slsm_osd);
		if (slsm.slsm_mbmp == NULL) {
		    printf("Importing bitmap failed\n");
		    free(slsm.slsm_osd, M_SLSMM);
		    bzero(&slsm, sizeof(slsm));
		    return EINVAL;
		}
	    }

	    slsm.slsm_proctable = hashinit(HASH_MAX, M_SLSMM, &slsm.slsm_procmask);
	    LIST_INIT(&slsm.slsm_snaplist);

	    slsm.slsm_cdev = 
		make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "sls");
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

	    if (slsm.slsm_proctable != NULL)
		hashdestroy(slsm.slsm_proctable, M_SLSMM, slsm.slsm_procmask);

	    if (slsm.slsm_osdvp != NULL) {
		VOP_CLOSE(slsm.slsm_osdvp, FREAD | FWRITE | O_NONBLOCK,
			curthread->td_proc->p_ucred, curthread);
		if (VOP_ISLOCKED(slsm.slsm_osdvp)) 
		    VOP_UNLOCK(slsm.slsm_osdvp, LK_RELEASE);
	    }

	    if (slsm.slsm_osd != NULL) {
		mbmp_free(slsm.slsm_mbmp);
		free(slsm.slsm_osd, M_SLSMM);
	    }

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

DECLARE_MODULE(sls_kmod, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
