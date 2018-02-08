#include "slsmm.h"

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/rwlock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/filio.h>

#include <machine/vmparam.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

MALLOC_DEFINE(M_SLSMM, "slsmm", "slsmm longer name");

static int
slsmm_dev_pager_ctor(void *handle, vm_ooffset_t size, vm_prot_t prot,
        vm_ooffset_t foff, struct ucred *cred, u_short *color)
{
    slsmm_object_t *vmh = handle;
    vmh->size = size;
    vmh->pages = malloc(sizeof(slsmm_page_t)*size, M_SLSMM, M_ZERO | M_WAITOK);
    if (color) *color = 0;
    dev_ref(vmh->dev);
    return 0;
}

static void
slsmm_dev_pager_dtor(void *handle)
{
    slsmm_object_t *vmh = handle;
    struct cdev *dev = vmh->dev;
    free(vmh->pages, M_SLSMM);
    free(vmh, M_SLSMM);
    dev_rel(dev);
}

slsmm_object_t *vmh;
static int
slsmm_dev_pager_fault(vm_object_t object, vm_ooffset_t offset, int prot, 
        vm_page_t *mres)
{
    vm_pindex_t pidx = OFF_TO_IDX(offset);
    slsmm_page_t page = vmh->pages[pidx]; 
    
    if (page.device == 0) {
        page.page = vm_page_lookup(object, pidx);
        page.device = 1;
    }

    if (*mres != page.page) {
        if (*mres != NULL) {
            vm_page_lock(*mres);
            vm_page_free(*mres);
            vm_page_unlock(*mres);
        }
        *mres = page.page;
    }
    page.page->valid = VM_PAGE_BITS_ALL;

    return  (VM_PAGER_OK);
}

static struct cdev_pager_ops slsmm_cdev_pager_ops = {
    .cdev_pg_ctor = slsmm_dev_pager_ctor,
    .cdev_pg_dtor = slsmm_dev_pager_dtor,
    .cdev_pg_fault = slsmm_dev_pager_fault,
};

    static int
slsmm_mmap_single(struct cdev *cdev, vm_ooffset_t *foff, vm_size_t objsize, 
        vm_object_t *objp, int prot)
{
    vm_object_t obj;

    vmh = malloc(sizeof(slsmm_object_t), M_SLSMM, M_NOWAIT | M_ZERO);
    if (vmh == NULL) return ENOMEM;
    vmh->dev = cdev;

    obj = cdev_pager_allocate(vmh, OBJT_MGTDEVICE, &slsmm_cdev_pager_ops, objsize, 
            prot, *foff, NULL);

    if (obj == NULL) {
        free(vmh, M_SLSMM);
        return EINVAL;
    }

    *objp = obj;
    return 0;
}

//=============================================================================

static struct cdev *slsmm_dev;

static d_read_t slsmm_read;
static d_write_t slsmm_write;
static d_ioctl_t slsmm_ioctl;

static char single;

static struct cdevsw slsmm_cdevsw = {
    .d_version =  D_VERSION,
    .d_read =	slsmm_read,
    .d_write =	slsmm_write,
    .d_ioctl =	slsmm_ioctl,
    .d_name =	"slsmm",
    // XXX: This prevents you from managing the memory
    // see vm_mmap_cdev in vm/vm_mmap.c
    // .d_flags =	D_MMAP_ANON,
    .d_mmap_single = slsmm_mmap_single,
};

static int
slsmm_read(struct cdev *dev __unused, struct uio *uio, int flags __unused) {
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

static int
slsmm_write(struct cdev *dev __unused, struct uio *uio, int flags __unused) {
    uio->uio_resid = 0;

    return (0);
}

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused,
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

static int
SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
    switch (inEvent) {
        case MOD_LOAD:
            uprintf("Load\n");
            slsmm_dev = make_dev_credf(MAKEDEV_ETERNAL_KLD, &slsmm_cdevsw, 0,
                    NULL, UID_ROOT, GID_WHEEL, 0666, "slsmm");
            return 0;

        case MOD_UNLOAD:
            uprintf("Unload\n");
            destroy_dev(slsmm_dev);
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
