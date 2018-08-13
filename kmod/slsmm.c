#include <sys/types.h>

#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

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


#include "_slsmm.h"
#include "cpuckpt.h"
#include "memckpt.h"
#include "slsmm.h"
#include "fileio.h"


MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
		int flag __unused, struct thread *td)
{
	int error = 0;
	struct slsmm_param *sparam;
	struct proc *p;
	int fd;

	struct proc_info *proc_info;
	struct thread_info *thread_info;

	struct vmspace *vmspace;
	vm_ooffset_t unused;

	struct timespec prev, curr;
	long nanos[5];

	nanotime(&curr);

	/* 
	 * Get a hold of the process to be 
	 * checkpointed, bringing its thread stack to memory
	 * and preventing it from exiting, to avoid race
	 * conditions.
	 */
	/*
	 * XXX Is holding appropriate here? Is it done correctly? It brings the stack
	 * into memory, but what about swapped out pages? 
	 */
	/*
	 * In general, a nice optimization would be to use swapped out pages as part of
	 * out checkpoint.
	 */
	sparam = (struct slsmm_param *) data;
	if (error) {
		printf("Error: copyin failed with code %d\n", error);
		return error;
	}

	fd = sparam->fd;
	error = pget(sparam->pid, PGET_WANTREAD, &p);
	if (error) {
		printf("Error: pget failed with code %d\n", error);
		return error;
	}

	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	switch (cmd) {
		case SLSMM_DUMP:
			printf("SLSMM_DUMP\n");

			prev = curr, nanouptime(&curr);

			proc_info = malloc(sizeof(struct proc_info), M_SLSMM, M_NOWAIT);
			if (proc_info == NULL) {
				printf("ENOMEM\n");
				return ENOMEM;
			}

			thread_info = malloc(sizeof(struct thread_info) * p->p_numthreads, 
					M_SLSMM, M_NOWAIT);
			if (thread_info == NULL) {
				printf("ENOMEM\n");
				return ENOMEM;
			}

			error = proc_checkpoint(p, proc_info);
			if (error) {
				printf("Error: proc_checkpoint failed with error code %d\n", error);
				break;
			}

			/* Dump the thread states*/
			/*
			 * XXX This error path, and error paths in general, is wrong.
			 * Must be fixed after we fix the happy path.
			 */
			error = thread_checkpoint(p, thread_info);
			if (error) {
				printf("Error: reg_checkpoint failed with error code %d\n", error);
				break;
			}

			prev = curr, nanouptime(&curr);
			nanos[0] = curr.tv_nsec - prev.tv_nsec;

			vmspace = vmspace_fork(p->p_vmspace, &unused);
			if (!vmspace) {
				printf("Error: vmspace_fork failed\n");
				break;
			}


			prev = curr, nanouptime(&curr);
			nanos[1] = curr.tv_nsec - prev.tv_nsec;

			/* Unlock the process ASAP to let it execute */
			kern_psignal(p, SIGCONT);
			PROC_UNLOCK(p);

			/* Release the hold we got when looking up the proc structure */
			PRELE(p);

			prev = curr, nanouptime(&curr);
			nanos[2] = curr.tv_nsec - prev.tv_nsec;

			/* Write dump to disk */
			fd_write(proc_info, sizeof(struct proc_info), fd);
			fd_write(thread_info, sizeof(struct thread_info) * p->p_numthreads, 
					fd);
			error = vmspace_checkpoint(vmspace, sparam->fd);
			if (error) {
				printf("Error: vmspace_checkpoint failed with error code %d\n", 
						error);
				break;
			}

			prev = curr, nanouptime(&curr);
			nanos[3] = curr.tv_nsec - prev.tv_nsec;

			vmspace_free(vmspace);
			free(thread_info, M_SLSMM);
			free(proc_info, M_SLSMM);

			uprintf("suspend\t%ld ns\n", nanos[0]);
			uprintf("cpu\t%ld ns\n", nanos[1]);
			uprintf("mem\t%ld ns\n", nanos[2]);
			uprintf("resume\t%ld ns\n", nanos[3]);
			uprintf("disk\t%ld ns\n", nanos[4]);

			break;

		case SLSMM_RESTORE:
			printf("SLSMM_RESTORE\n");

			/* Bring process- and thread-related data to disk */
			proc_info = malloc(sizeof(struct proc_info), M_SLSMM, M_NOWAIT);
			if (proc_info == NULL) {
				printf("ENOMEM\n");
				return ENOMEM;
			}

			error = fd_read(proc_info, sizeof(struct proc_info), fd);
			if (error) {
				return error;
			}

			thread_info = malloc(sizeof(struct thread_info) * proc_info->nthreads, M_SLSMM, M_NOWAIT);
			if (thread_info == NULL) {
				free(proc_info, M_SLSMM);
				printf("ENOMEM\n");
				return ENOMEM;
			}

			error = fd_read(thread_info, sizeof(struct thread_info) * proc_info->nthreads, fd);
			if (error) {
				free(thread_info, M_SLSMM);
				return error;
			}

			/* Carry on with the restore */
			error = proc_restore(p, proc_info);
			if (error) {
				printf("Error: reg_restore failed with error code %d\n", error);
				break;
			}

			error = thread_restore(p, thread_info);
			if (error) {
				printf("Error: reg_restore failed with error code %d\n", error);
				break;
			}


			error = vmspace_restore(p, fd);
			if (error) {
				printf("Error: vmspace_restore failed with error code %d\n", error);
				break;
			}

			kern_psignal(p, SIGCONT);
			PROC_UNLOCK(p);

			/* Release the hold we got when looking up the proc structure */
			PRELE(p);

			break;
	}

	printf("Error code %d\n", error);
	return error;
}

static struct cdevsw slsmm_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl = slsmm_ioctl,
};
static struct cdev *slsmm_dev;

static int
SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
	int error = 0;
	switch (inEvent) {
		case MOD_LOAD:
			printf("Loaded\n");
			slsmm_dev = make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "slsmm");
			break;
		case MOD_UNLOAD:
			printf("Unloaded\n");
			destroy_dev(slsmm_dev);
			break;
		default:
			error = EOPNOTSUPP;
			break;
	}
	return error;
}

static moduledata_t moduleData = {
	"slsmm",
	SLSMMHandler,
	NULL
};


DECLARE_MODULE(slsmm_kmod, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
