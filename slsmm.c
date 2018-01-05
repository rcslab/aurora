#include "slsmm.h"

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/filio.h>

#include <machine/vmparam.h>

static struct cdev *zero_dev;

static d_write_t null_write;
static d_ioctl_t zero_ioctl;
static d_read_t zero_read;

static struct cdevsw zero_cdevsw = {
    .d_version =  D_VERSION,
    .d_read =	zero_read,
    .d_write =	null_write,
    .d_ioctl =	zero_ioctl,
    .d_name =	"slsmm",
    .d_flags =	D_MMAP_ANON,
};

static int zero_read(struct cdev *dev __unused, struct uio *uio, int flags __unused) {
    void *zbuf;
    ssize_t len;
    int error = 0;

    KASSERT(uio->uio_rw == UIO_READ,
            ("Can't be in %s for write", __func__));
    zbuf = __DECONST(void *, zero_region);
    while (uio->uio_resid > 0 && error == 0) {
        len = uio->uio_resid;
        if (len > ZERO_REGION_SIZE)
            len = ZERO_REGION_SIZE;
        error = uiomove(zbuf, len, uio);
    }

    return (error);
}

static char single;

static int zero_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused,
        int flags __unused, struct thread *td) {
    int error;
    error = 0;

    switch (cmd) {
        case SLSMM_WRITE:
            single = *(char *)data;
            break;
        case SLSMM_READ:
            *(char *)data = single;
        case FIONBIO:
            break;
        case FIOASYNC:
            if (*(int *)data != 0)
                error = EINVAL;
            break;
        default:
            error = ENOIOCTL;
    }
    return (error);
}

static int null_write(struct cdev *dev __unused, struct uio *uio, int flags __unused) {
    uio->uio_resid = 0;

    return (0);
}

static int SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
    switch (inEvent) {
        case MOD_LOAD:
            uprintf("Load\n");
            zero_dev = make_dev_credf(MAKEDEV_ETERNAL_KLD, &zero_cdevsw, 0,
                    NULL, UID_ROOT, GID_WHEEL, 0666, "slsmm");
            return 0;

        case MOD_UNLOAD:
            uprintf("Unload\n");
            destroy_dev(zero_dev);
            return 0;

        default:
            return (EOPNOTSUPP);
    }
}

static moduledata_t moduleData = {
    "slsmm",
    SLSMMHandler,
    NULL
};

DECLARE_MODULE(slsmm_kmod, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
