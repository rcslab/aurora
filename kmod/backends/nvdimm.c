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
#include "../hash.h"

void *nvdimm = NULL;
size_t nvdimm_size;
uint64_t nvdimm_offset = 0;
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
    nvdimm_size = ((struct SPA_mapping *) nvdimm_cdev->si_drv1)->spa_len;

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
    else 
	free(nvdimm, M_SLSMM);
}


int
nvdimm_read(void *addr, size_t len, vm_offset_t offset)
{
    uintptr_t nvdimm_cur;

    if (nvdimm == NULL) {
	printf("nvdimm is not open\n");
	return 0;
    }

    if (nvdimm_offset + len > (vm_offset_t) nvdimm_size) {
	printf("NVDIMM overflow\n");
	return 0;
    }

    nvdimm_cur = (uintptr_t) nvdimm + nvdimm_offset;

    bcopy((void *) nvdimm_cur, addr, len);
    nvdimm_offset += len;

    return 0;
}

/*
 * XXX Use worker-specific offsets 
 */
int
nvdimm_write(void *addr, size_t len, vm_offset_t offset)
{
    uintptr_t nvdimm_cur;

    if (nvdimm == NULL) {
	printf("nvdimm is not open\n");
	return 0;
    }

    if (nvdimm_offset + len > (vm_offset_t) nvdimm_size) {
	printf("NVDIMM overflow\n");
	return 0;
    }

    nvdimm_cur = (uintptr_t) nvdimm + nvdimm_offset;

    bcopy(addr, (void *) nvdimm_cur, len);

    nvdimm_offset += len;
    sls_log[7][sls_log_counter] += len;


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
	int cnt = 0;
	struct dump_page *dump_page;
	vm_offset_t offset;
	vm_object_t obj;
	uintptr_t lineno;
	struct timespec tflush, tflushend;


	worker_index = 0;
	worker = &sls_workers[worker_index];
	mtx_lock(&worker->work_mtx);
	for (i = 0; i < numentries; i++) {

	    if (objects[i] == NULL)
		continue;

	    entry = &entries[i];
	    offset = entry->offset;

	    obj = objects[i];
	    while (obj != NULL) {
		/* 
		 * We reached enough into the tree that 
		 * we have non-anonymous objects, so break.
		 */
		if (obj->type != OBJT_DEFAULT)
		    break;

		TAILQ_FOREACH(page, &objects[i]->memq, listq) {

		    /*
		    * XXX Does this check make sense? We _are_ getting pages
		    * from a valid object, after all, why would it have NULL
		    * pointers in its list? We could turn it into a KASSERT.
		    */
		    if (!page) {
			printf("ERROR: vm_page_t page is NULL");
			continue;
		    }

		    /* 
		     * XXX reuse dump_page structures, don't alloc/free constantly.
		     * Maybe keep a pool?
		     */

		    /* XXX It that a good idea? */
		    dump_page = malloc(sizeof(*dump_page), M_SLSMM, M_NOWAIT);
		    if (dump_page == NULL) {
			/* XXX error handling */
		    }

		    vaddr_data = IDX_TO_VADDR(page->pindex, entry->start, offset);
		    dump_page->vaddr = vaddr_data;
		    dump_page->page = page;


		    LIST_INSERT_HEAD(&worker->work_list, dump_page, next);

		    /* XXX Tune batche size sent to the worker */
		    cnt++;
		    if (cnt % 64) {
			/* Wake up the thread */
			cv_signal(&worker->work_cv);
			mtx_unlock(&worker->work_mtx);

			/* 
			 * XXX Right now we have only one worker, but later
			 * we can multithread it.
			 */
			worker_index = (worker_index + 1) % WORKER_THREADS;
			worker = &sls_workers[worker_index];
			mtx_lock(&worker->work_mtx);
		    }
		}

		/* Traverse the backing object */
		offset += obj->backing_object_offset;
		obj = obj->backing_object;
	    }
	}
	cv_signal(&worker->work_cv);
	mtx_unlock(&worker->work_mtx);


	/* 
	 * Busy wait until all the workers drain their queues. There is
	 * no one else to wake them back up.
	 */
	for (i = 0; i < WORKER_THREADS; i++) {
	    worker = &sls_workers[i];
	    while (!TD_IS_SLEEPING(worker->work_td))
		pause_sbt("slsdmp", SBT_1MS, 0 , C_HARDCLOCK | C_CATCH);
	}
	
	/* Make instrumenting the code for performance modular */
	nanotime(&tflush);

	/* 
	 * Flush the LLC to the NVDIMM. Do it all at once here; 
	 * That way, for large checkpoints most data has already
	 * been evicted.
	 */
	for (lineno = 0; lineno < nvdimm_offset / 64; lineno++) {
	    lineno = (uintptr_t) nvdimm - ((uintptr_t) nvdimm % 64) + lineno * 64;
	    clflush(lineno);
	}

	nanotime(&tflushend);

	sls_log[8][sls_log_counter] = tonano(tflushend) - tonano(tflush);

}
