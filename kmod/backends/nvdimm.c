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
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>

#include "fileio.h"
#include "nvdimm.h"
#include "worker.h"
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

void
nvdimm_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
	    size_t numentries, void *addr)
{
	int worker_index;
	vm_page_t page;
	vm_offset_t vaddr_data;
	struct sls_worker *worker;
	struct vm_map_entry_info *entry;
	int i;

	/*
	 * XXX Turn into a workers_activate function
	 */
	/* Wake up the threads */
	for (i = 0; i < WORKER_THREADS; i++) {
	    worker = &sls_workers[i];
	    worker->work_offset = 0;
	    mtx_lock(&worker->work_mtx);
	    cv_signal(&worker->work_cv);
	    mtx_unlock(&worker->work_mtx);
	}

	/*
	 * XXX Turn into a workers_activate function
	 */
	/* Wake up the threads */
	for (i = 0; i < WORKER_THREADS; i++) {
	    worker = &sls_workers[i];
	    worker->work_offset = 0;
	    mtx_lock(&worker->work_mtx);
	    cv_signal(&worker->work_cv);
	    mtx_unlock(&worker->work_mtx);
	}


	worker_index = 0;
	worker = &sls_workers[worker_index];
	for (i = 0; i < numentries; i++) {

	    if (objects[i] == NULL)
		continue;

	    entry = &entries[i];

	    TAILQ_FOREACH(page, &objects[i]->memq, listq) {

		/*
		* XXX Does this check make sense? We _are_ getting pages
		* from a valid object, after all, why would it have NULL
		* pointers in its list?
		*/
		if (!page) {
		    printf("ERROR: vm_page_t page is NULL");
		    continue;
		}

		vaddr_data = IDX_TO_VADDR(page->pindex, entry->start, entry->offset);

		/* XXX Race condition if we modify the page before the worker reads it */
		worker->work_page = page;
		while (atomic_cmpset_ptr(&worker->work_offset, 0, vaddr_data) == 0)
		    /* Keep trying */; 

		worker_index = (worker_index + 1) % WORKER_THREADS;
		worker = &sls_workers[worker_index];

	    }

	    /* For delta dumps, deallocate the object to collapse the chain again. */
	}

	/* Tell the worker threads to stop */
	for (i = 0; i < WORKER_THREADS; i++) {
		/* Stop using atomics */
		worker = &sls_workers[i];
		while (atomic_cmpset_ptr(&worker->work_offset, 0, SLS_POISON))
		    /* Keep trying */; 
	}

	/* Wait until they all do */
	for (i = 0; i < WORKER_THREADS; i++) {
	    /* If even one of them is awake, try again from the beginning */
	    worker = &sls_workers[i];
	    if (!TD_IS_SLEEPING(worker->work_td))
		    i = 0;
	}

}
