#include <sys/param.h>

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>

#include "fileio.h"
#include "nvdimm.h"
#include "../memckpt.h"
#include "../_slsmm.h"
#include "../slsmm.h"

void *nvdimm = NULL;
static uint64_t nvdimm_len = 0;
static uint64_t nvdimm_offset = 0;
static struct cdev *nvdimm_cdev = NULL;
static int nvdimm_ref = 0;

int
nvdimm_open(void)
{
    struct cdevsw *dsw;
    struct nameidata nameidata;
    struct vnode *vp;
    int error;
    int nvdimm_ref = 0;


    NDINIT(&nameidata, LOOKUP, FOLLOW, UIO_SYSSPACE, NVDIMM_NAME, curthread);
    error = namei(&nameidata);
    if (error) {
	printf("Error: namei for path failed with %d\n", error);
	return error;
    }

    vp = nameidata.ni_vp;
    if (vp == NULL) {
	/* It's a no-op I think, since we don't pass SAVENAME */
	NDFREE(&nameidata, NDF_ONLY_PNBUF);

	return ENOENT;
    }

    if (vp->v_type != VBLK && vp->v_type != VCHR)
	return EINVAL;


    nvdimm_cdev = vp->v_rdev;
    dsw = dev_refthread(nvdimm_cdev, &nvdimm_ref);
    if (dsw == NULL) {
	NDFREE(&nameidata, NDF_ONLY_PNBUF);
	return ENXIO;
    }

    nvdimm = ((struct SPA_mapping *) nvdimm_cdev->si_drv1)->spa_kva;
    nvdimm_len = ((struct SPA_mapping *) nvdimm_cdev->si_drv1)->spa_len;

    if (nvdimm == NULL)
	printf("WARNING: SPA kernel virtual address is null\n");

    NDFREE(&nameidata, NDF_ONLY_PNBUF);

    return 0;
}

void
nvdimm_close(void)
{
    if (nvdimm_cdev != NULL)
        dev_relthread(nvdimm_cdev, nvdimm_ref);
}


int
nvdimm_read(void *addr, size_t len, vm_offset_t offset)
{
    return 0;
}

/*
 * XXX Write semantics as in userspace? If yes, do
 * we add a - sign next to the error codes for errors?
 */
int
nvdimm_write(void *addr, size_t len, vm_offset_t offset)
{
    if (nvdimm == NULL) {
	printf("nvdimm is not open\n");
	return 0;
    }

    if (nvdimm_offset + len > (vm_offset_t) nvdimm) {
	printf("NVDIMM overflow\n");
	return 0;
    }

    bcopy(addr, nvdimm, len);

    return 0;
}
