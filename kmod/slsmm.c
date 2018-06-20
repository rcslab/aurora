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

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused,
        int flag __unused, struct thread *td)
{
    int error = 0;
    struct dump_param *dparam;
    struct restore_param *rparam;
    struct proc *p;

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

            PROC_LOCK(p);
            kern_psignal(p, SIGSTOP);
            PROC_UNLOCK(p);
            uprintf("suspended\n");

            error = reg_dump(p, dparam->fd);
            printf("cpu error %d\n", error);
            error = vmspace_dump(p->p_vmspace, dparam->start, dparam->end, td, dparam->fd);
            printf("mem error %d\n", error);

            PROC_LOCK(p);
            kern_psignal(p, SIGCONT);
            PROC_UNLOCK(p);
            uprintf("unsuspended\n");

            if (dparam->pid != -1) PRELE(p);

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
