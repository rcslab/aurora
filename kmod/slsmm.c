#include "_slsmm.h"
#include "slsmm.h"
#include "fileio.h"
#include "cpuckpt.h"
#include "memckpt.h"

#include <sys/conf.h>       // cdev
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused,
        int flag __unused, struct thread *td)
{
    int error = 0;
    struct dump_param *dparam;
    struct restore_param *rparam;
    struct proc *p;

    void *reg_buffer = NULL;
    size_t reg_dump_size;

    vm_object_t *objects = NULL;
    struct vm_map_entry_info *entries = NULL;
    size_t nentries = 0;

    struct timespec prev, curr;
    nanotime(&curr);
    long nanos[5];

    switch (cmd) {
        case SLSMM_DUMP:
            printf("SLSMM_DUMP\n");
            dparam = (struct dump_param *)data;

            if (dparam->pid == -1) {
                p = td->td_proc;
            } else {
                error = pget(dparam->pid, PGET_WANTREAD, &p);
                if (error) break;
            }

            // suspend process
            prev = curr, nanouptime(&curr);
            PROC_LOCK(p);
            kern_psignal(p, SIGSTOP);
            PROC_UNLOCK(p);
            prev = curr, nanouptime(&curr);
            nanos[0] = curr.tv_nsec - prev.tv_nsec;

            // dump cpu registers
            error = reg_dump(&reg_buffer, &reg_dump_size, p, dparam->fd);
            if (error) break;
            prev = curr, nanouptime(&curr);
            nanos[1] = curr.tv_nsec - prev.tv_nsec;

            // create memory shadow objects
            error = shadow_object_list(p->p_vmspace, &objects, &entries, &nentries);
            if (error) break;
            prev = curr, nanouptime(&curr);
            nanos[2] = curr.tv_nsec - prev.tv_nsec;

            // resume process
            PROC_LOCK(p);
            kern_psignal(p, SIGCONT);
            PROC_UNLOCK(p);
            if (dparam->pid != -1) PRELE(p);
            prev = curr, nanouptime(&curr);
            nanos[3] = curr.tv_nsec - prev.tv_nsec;

            // write cpu and mem dump to disk
            fd_write(reg_buffer, reg_dump_size, dparam->fd);
            vm_objects_dump(p->p_vmspace, objects, entries, nentries, dparam->fd);
            prev = curr, nanouptime(&curr);
            nanos[4] = curr.tv_nsec - prev.tv_nsec;

            if (reg_buffer) free(reg_buffer, M_SLSMM);
            if (objects) free(objects, M_SLSMM);
            if (entries) free(entries, M_SLSMM);

            uprintf("suspend\t%ld ns\n", nanos[0]);
            uprintf("cpu\t%ld ns\n", nanos[1]);
            uprintf("mem\t%ld ns\n", nanos[2]);
            uprintf("resume\t%ld ns\n", nanos[3]);
            uprintf("disk\t%ld ns\n", nanos[4]);

            break;

        case SLSMM_RESTORE:
            printf("SLSMM_RESTORE\n");
            rparam = (struct restore_param *)data;

            if (rparam->pid == -1) {
                p = td->td_proc;
            } else {
                error = pget(rparam->pid, PGET_WANTREAD, &p);
                if (error) break;
            }

            PROC_LOCK(p);
            kern_psignal(p, SIGCONT);
            PROC_UNLOCK(p);
            uprintf("unsuspended\n");
            break;

            break;
    }

    printf("ret error %d\n", error);
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
