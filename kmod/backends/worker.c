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
#include "worker.h"
#include "../memckpt.h"
#include "../_slsmm.h"
#include "../slsmm.h"
#include "nvdimm.h"

/* XXX turns out these belong exclusively to the OSD */

struct sls_worker sls_workers[WORKER_THREADS];
extern int time_to_die;

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
    vm_offset_t index;
    struct dump_page *dump_page;
    vm_offset_t vaddr_data;

    worker = (struct sls_worker *) arg;
    mtx = &worker->work_mtx;
    cv = &worker->work_cv;
    id = worker->work_id;
    index = worker->work_index;
    td = worker->work_td;

    while (time_to_die == 0) {
	mtx_lock(mtx);

	while ((dump_page = LIST_FIRST(&worker->work_list)) == NULL)
	    cv_wait(cv, mtx);

	LIST_REMOVE(dump_page, next);
	page = dump_page->page;
	vaddr_data = dump_page->vaddr;

	error = nvdimm_write(&vaddr_data, sizeof(vm_offset_t), index);
	if (error != 0) {
	    printf("Error: writing vm_map_entry_info failed\n");
	    continue;
	}

	vaddr = userpage_map(page->phys_addr);
	if ((void *) vaddr == NULL) {
	    printf("Mapping page failed\n");
	    continue;
	}

	/* XXX Account for superpages */
	error = nvdimm_write((void*) vaddr, PAGE_SIZE, index);
	if (error != 0) {
	    printf("Error: write failed with %d\n", error);
	    continue;
	}
	worker->work_index += PAGE_SIZE;

	userpage_unmap(vaddr);
	worker->cnt++;

	free(dump_page, M_SLSMM);
	mtx_unlock(mtx);
    }

    worker->work_id = -1;

    mtx_lock(mtx);
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

	printf("Initializing list\n");
	LIST_INIT(&worker->work_list);
	printf("List initialized\n");
	cv_init(&worker->work_cv, "slsworkercv");
	mtx_init(&worker->work_mtx, "slsworkedmtx", NULL, MTX_DEF);
	worker->work_id = i;
	worker->cnt = 0;

	/* Since we're using the OSD as an array, space workers equally */
	worker->work_index = (vm_offset_t) nvdimm + i * (nvdimm_size / WORKER_THREADS);
	printf("worker index is %lx\n", worker->work_index);

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
    
    /* 
     * XXX Implement proper locking to eliminate
     * races between loads/unloads and dumps/restores
     */
}

