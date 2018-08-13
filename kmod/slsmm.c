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

    struct cpu_info *reg_buffer = NULL;
    size_t reg_dump_size;

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

	    /* Dump the CPU registers */
	    /*
	     * XXX This error path, and error paths in general, is wrong.
	     * Must be fixed after we fix the happy path.
	     */
            error = reg_dump(&reg_buffer, &reg_dump_size, p, fd);
            if (error) {
		    printf("Error: reg_dump failed with error code %d\n", error);
		    break;
	    }

            prev = curr, nanouptime(&curr);
            nanos[0] = curr.tv_nsec - prev.tv_nsec;

	    vmspace = vmspace_fork(p->p_vmspace, &unused);
	    if (!vmspace) {
	        printf("Error: vmspace could not be forked\n");
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
            fd_write(reg_buffer, reg_dump_size, fd);
	    error = vmspace_checkpoint(vmspace, sparam->fd);
            if (error) {
		    printf("Error: vmspace_checkpoint failed with error code %d\n", error);
		    break;
	    }

            prev = curr, nanouptime(&curr);
            nanos[3] = curr.tv_nsec - prev.tv_nsec;

            if (reg_buffer) 
		    free(reg_buffer, M_SLSMM);
	    vmspace_free(vmspace);

            uprintf("suspend\t%ld ns\n", nanos[0]);
            uprintf("cpu\t%ld ns\n", nanos[1]);
            uprintf("mem\t%ld ns\n", nanos[2]);
            uprintf("resume\t%ld ns\n", nanos[3]);
            uprintf("disk\t%ld ns\n", nanos[4]);

            break;

        case SLSMM_RESTORE:
            printf("SLSMM_RESTORE\n");

	    error = reg_restore(p, fd);
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
