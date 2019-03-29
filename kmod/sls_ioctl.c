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
#include "sls_process.h"
#include "sls_ioctl.h"

#include "sls_dump.h"


MALLOC_DEFINE(M_SLSMM, "slsmm", "SLSMM");


struct sls_metadata sls_metadata;

static int
slsmm_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
	int flag __unused, struct thread *td)
{
	struct op_param *oparam = NULL;
	struct compose_param *cparam = NULL;
	struct slsp_param *sparam = NULL;

	struct sls_op_args *op_args;
	struct proc *p = NULL;

	char *snap_name = NULL;
	size_t snap_name_len;

	int *ids = NULL;
	int dump_mode;
	int error = 0;
	int fd_type;
	int op;

	switch (cmd) {
	    case SLS_OP:

		oparam = (struct op_param *) data;

		fd_type = oparam->fd_type;
		op = oparam->op;

		if (op != SLS_CHECKPOINT && oparam->dump_mode != SLS_FULL) {
		    printf("Error: Dump mode flag only valid with checkpointing\n");
		    return EINVAL;
		}

		if (fd_type != SLS_FD_MEM && fd_type != SLS_FD_FILE) {
		    printf("Error: Invalid checkpoint fd type %d\n", fd_type);
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
		if (oparam->dump_mode == SLS_FULL)
		    dump_mode = SLS_CKPT_FULL;
		else 
		    dump_mode = SLS_CKPT_DELTA;


		if (fd_type == SLS_FD_FILE) {
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
		op_args->fd_type = oparam->fd_type;
		op_args->dump_mode = dump_mode;
		op_args->iterations = oparam->iterations;
		op_args->interval = oparam->period;

		if (op == SLS_CHECKPOINT) {
		    error = kthread_add((void(*)(void *)) sls_checkpointd, 
			op_args, NULL, NULL, 0, 0, "sls_checkpointd");
		} else if (op == SLS_RESTORE) {
		    error = kthread_add((void(*)(void *)) sls_restored, 
			op_args, p, NULL, 0, 0, "sls_restored");

		    kern_thr_exit(curthread);
		}

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

	    case SLS_SLSP:
		sparam = (struct slsp_param *) data;

		switch (sparam->op) {
		case SLS_PLIST:
		    slsp_list();
		    return 0;

		case SLS_PDEL:
		    slsp_delete(sparam->id);
		    return 0;

		default:
		    return EINVAL;
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

		snprintf(buf, 1024, "%u,%lu\n", j, sls_metadata.slsm_log[j][i]);

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
    
    switch (inEvent) {
	case MOD_LOAD:
	    printf("SLS Loaded.\n");
	    sls_metadata.slsm_exiting = 0;

	    TAILQ_INIT(&sls_procs);
	    printf("Size of array: %lu\n", sizeof(sls_metadata.slsm_ckptd));
	    bzero(sls_metadata.slsm_ckptd, sizeof(sls_metadata.slsm_ckptd));

	    sls_metadata.slsm_lastid = 0;
	    sls_metadata.slsm_cdev = 
		make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "slsmm");

	    break;

	case MOD_UNLOAD:

	    sls_metadata.slsm_exiting = 1;

	    slsp_delete_all();

	    destroy_dev(sls_metadata.slsm_cdev);

	    printf("SLS Unloaded.\n");
	    break;
	default:
	    error = EOPNOTSUPP;
	    break;
    }
    return error;
}



static moduledata_t moduleData = {
    "slsmm",
    SLSHandler,
    NULL
};

DECLARE_MODULE(slsmm_kmod, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
