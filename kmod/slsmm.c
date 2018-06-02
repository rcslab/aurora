#include "_slsmm.h"
#include "slsmm.h"
#include "fileio.h"
#include "cpuckpt.h"
#include "memckpt.h"

#include <sys/conf.h>       // cdev
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/systm.h>

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused,
        int flag __unused, struct thread *td)
{
    int error = 0;
    struct dump_param *param;
    struct proc *p;

    switch (cmd) {
        case SLSMM_DUMP:
            printf("SLSMM_DUMP\n");
            param = (struct dump_param *)data;

            if (param->pid == -1) {
                p = td->td_proc;
            } else {
                error = pget(param->pid, PGET_WANTREAD, &p);
                if (error) break;
            }

            error = reg_dump(p, param->fd);
            printf("cpu error %d\n", error);
            error = vmspace_dump(p->p_vmspace, param->start, param->end, td, param->fd);
            printf("mem error %d\n", error);

            break;

        case SLSMM_RESTORE:
            printf("SLSMM_RESTORE\n");
            param = (struct dump_param *)data;

            if (param->pid == -1) {
                p = td->td_proc;
            } else {
                error = pget(param->pid, PGET_WANTREAD, &p);
                if (error) break;
            }

            error = reg_restore(p, param->fd);
            printf("cpu error %d\n", error);
            error = vmspace_restore(p, td, param->fd);
            printf("mem error %d\n", error);

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
