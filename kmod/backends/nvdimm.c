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
#include "../memckpt.h"
#include "../_slsmm.h"
#include "../slsmm.h"
#include "nvdimm.h"

struct sls_worker sls_workers[WORKER_THREADS];
extern int time_to_die;
void *nvdimm = NULL;
size_t nvdimm_size;
static struct cdev *nvdimm_cdev = NULL;
static int nvdimm_ref = 0;
static uma_zone_t sls_pagechunk_zone;

static
void sls_writed(void *arg)
{
    vm_offset_t vaddr;
    vm_page_t page;
    int error;
    struct sls_worker *worker;
    long id;
    struct mtx *mtx;
    struct cv *cv;
    struct thread *td;
    struct sls_pagechunk *pagechunk;
    vm_offset_t vaddr_data;
    uintptr_t dest;
    int i;

    worker = (struct sls_worker *) arg;
    mtx = &worker->work_mtx;

    cv = &worker->work_cv;
    id = worker->work_id;
    td = worker->work_td;

    for (;;) {
	mtx_lock(mtx);
	if (time_to_die != 0)
	    break;

	while ((pagechunk = LIST_FIRST(&worker->work_list)) == NULL) {
	    worker->work_in_progress = 0;
	    cv_wait(cv, mtx);
	}
	LIST_REMOVE(pagechunk, next);
	mtx_unlock(mtx);


	for (i = 0; i < pagechunk->size; i++) {
	    vaddr_data = pagechunk->pagedata[i].vaddr;
	    page = pagechunk->pagedata[i].page;
	    dest = pagechunk->pagedata[i].dest;

	    error = nvdimm_write(&vaddr_data, sizeof(vm_offset_t), &dest);
	    if (error != 0) {
		printf("Error: writing vm_map_entry_info failed\n");
		continue;
	    }
	    dest += sizeof(vm_offset_t);

	    vaddr = userpage_map(page->phys_addr);
	    if ((void *) vaddr == NULL) {
		printf("Mapping page failed\n");
		continue;
	    }

	    /* XXX Account for superpages */
	    error = nvdimm_write((void*) vaddr, PAGE_SIZE, &dest);
	    if (error != 0) {
		printf("Error: write failed with %d\n", error);
		continue;
	    }

	    userpage_unmap(vaddr);
	}

	uma_zfree(sls_pagechunk_zone, pagechunk);
    }

    worker->work_id = -1;

    cv_signal(cv);
    mtx_unlock(mtx);

    printf("Exiting.\n");
    kthread_exit();
}

int
sls_workers_init(void)
{
    int error;
    int i;
    struct sls_worker *worker;

    /* 
	* TEMP: This code is, I think, proof that the 
	* witness module is reporting a false positive.
	*/
        /*
	struct mtx *worker_mtx = &sls_mtx[0];
	mtx_lock(worker_mtx);
	mtx_assert(worker_mtx, MA_LOCKED);
	cv_wait_unlock(&sls_cv[0], worker_mtx);
	printf("Assert passed here goddammit\n");
	*/

    for (i = 0; i < WORKER_THREADS; i++) {
	worker = &sls_workers[i];

	LIST_INIT(&worker->work_list);
	cv_init(&worker->work_cv, "slsworkercv");
	mtx_init(&worker->work_mtx, "slsworkedmtx", NULL, MTX_DEF);
	worker->work_id = i;
	worker->work_in_progress = 0;

	error = kthread_add((void(*)(void *)) sls_writed, (void *) worker,
	      NULL, &worker->work_td, 0, 0, "slsworkerd%d", i);
	if (error) {
	    printf("kthread_add for sls_writed failed with %d\n", error);
	    /* XXX Cleanup */
	}
    }

    return 0;
}

void
sls_workers_destroy(void)
{
    int i;
    struct mtx *mtx;
    struct cv *cv;

    for (i = 0; i < WORKER_THREADS; i++) {
	mtx = &sls_workers->work_mtx;
	cv = &sls_workers->work_cv;

	mtx_lock(mtx);
	if (sls_workers[i].work_id >=  0) {
	    printf("Sleeping.\n");
	    cv_wait(cv, mtx);
	}
	printf("Continuing.\n");

	mtx_unlock(mtx);

	cv_destroy(cv);
	mtx_destroy(mtx);

    }
}


int
nvdimm_open(void)
{
    struct cdevsw *dsw;
    struct nameidata nameidata;
    struct vnode *vp;
    int error;
    int nvdimm_ref = 0;

    /* XXX error handling */
    sls_pagechunk_zone = uma_zcreate("SLS dump pages", sizeof(struct sls_pagechunk),
		NULL, NULL, NULL, NULL, 
		UMA_ALIGNOF(struct dump_page), 0);


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
if (nvdimm == NULL) printf("WARNING: SPA kernel virtual address is null\n");

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

    uma_zdestroy(sls_pagechunk_zone);
}


int
nvdimm_read(void *dest, size_t len, vm_offset_t *offset)
{
    if (nvdimm == NULL) {
	printf("nvdimm is not open\n");
	return 0;
    }

    if (*offset + len > (uintptr_t) nvdimm +  nvdimm_size) {
	printf("NVDIMM overflow\n");
	return 0;
    }

    bcopy((void *) *offset, dest, len);
    *offset = *offset + len;

    return 0;
}

/*
 * XXX Use worker-specific offsets 
 */
int
nvdimm_write(void *src, size_t len, vm_offset_t *offset)
{
    if (nvdimm == NULL) {
	printf("nvdimm is not open\n");
	return 0;
    }

    if (*offset + len > (uintptr_t) nvdimm +  nvdimm_size) {
	printf("NVDIMM overflow\n");
	return 0;
    }

    bcopy(src, (void *) *offset, len);
    *offset = *offset + len;


    return 0;
}



void
nvdimm_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
	    size_t numentries, uintptr_t *addr, int mode)
{
	int worker_index;
	vm_page_t page;
	vm_offset_t vaddr_data;
	struct sls_worker *worker;
	struct vm_map_entry_info *entry;
	int i;
	struct sls_pagechunk *pagechunk;
	vm_offset_t offset;
	vm_object_t obj;
	uintptr_t lineno;
	struct timespec tflush, tflushend;
	int index = 0;


	worker_index = 0;
	worker = &sls_workers[worker_index];
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

		TAILQ_FOREACH(page, &obj->memq, listq) {

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
		     * If index is 0, we are either starting or just filled 
		     * a chunk, so go get a new one.
		     */
		    if (index == 0) {
			pagechunk = uma_zalloc(sls_pagechunk_zone, M_NOWAIT);
			if (pagechunk == NULL) {
			    /* XXX error handling */
			}
		    }

		    vaddr_data = IDX_TO_VADDR(page->pindex, entry->start, offset);
		    pagechunk->pagedata[index].vaddr = vaddr_data;
		    pagechunk->pagedata[index].dest = *addr;
		    pagechunk->pagedata[index].page = page;

		    *addr += sizeof(vm_offset_t) + PAGE_SIZE;

		    index = (index + 1) % PAGECHUNK_SIZE;
		    if (index == 0) {
			pagechunk->size = PAGECHUNK_SIZE;

			/* Filled up, send it */
			mtx_lock(&worker->work_mtx);
			LIST_INSERT_HEAD(&worker->work_list, pagechunk, next);
			worker->work_in_progress = 1;

			cv_signal(&worker->work_cv);
			mtx_unlock(&worker->work_mtx);

			/* 
			 * XXX Right now we have only one worker, but later
			 * we can multithread it.
			 */
			worker_index = (worker_index + 1) % WORKER_THREADS;
			worker = &sls_workers[worker_index];
		    }
		}


		/* Traverse the backing object only if doing full dumps */
		if (mode == SLSMM_CKPT_DELTA)
		    break;

		offset += obj->backing_object_offset;
		obj = obj->backing_object;
	    }
	}

	pagechunk->size = index;
	if (index != 0) {
	    /* Send the last chunk, even if half-filled */
	    mtx_lock(&worker->work_mtx);
	    LIST_INSERT_HEAD(&worker->work_list, pagechunk, next);
	    worker->work_in_progress = 1;

	    cv_signal(&worker->work_cv);
	    mtx_unlock(&worker->work_mtx);
	}

	/* Make instrumenting the code for performance modular */
	nanotime(&tflush);

	/* 
	 * Busy wait until all the workers drain their queues. There is
	 * no one else to wake them back up.
	 */
	for (i = 0; i < WORKER_THREADS; i++) {
	    worker = &sls_workers[i];
	    for (;;) {
		mtx_lock(&worker->work_mtx);

		if (worker->work_in_progress == 0)
		    break;

		mtx_unlock(&worker->work_mtx);

		pause_sbt("slsdmp", SBT_1MS, 0 , C_HARDCLOCK | C_CATCH);
	    }
	    mtx_unlock(&worker->work_mtx);
	}
	
	nanotime(&tflushend);

	/* 
	 * Flush the LLC to the NVDIMM. Do it all at once here; 
	 * That way, for large checkpoints most data has already
	 * been evicted.
	 */
	for (lineno = 0; lineno < (*addr - (uintptr_t) nvdimm) / 64; lineno++) {
	    lineno = (uintptr_t) nvdimm - ((uintptr_t) nvdimm % 64) + lineno * 64;
	    clflush(lineno);
	}


	sls_log[7][sls_log_counter] = *addr - (uintptr_t) nvdimm;
	sls_log[8][sls_log_counter] = tonano(tflushend) - tonano(tflush);

}
