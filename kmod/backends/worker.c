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


struct sls_worker sls_workers[WORKER_THREADS];

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
    vm_offset_t *offp;
    struct sls_desc desc;

    worker = (struct sls_worker *) arg;
    mtx = &worker->work_mtx;
    cv = &worker->work_cv;
    id = worker->work_id;
    td = worker->work_td;
    offp = &worker->work_offset;
    desc = worker->work_desc;

    /* XXX Check if we need to exit */ 
    for (;;) {
	mtx_lock(mtx);
	cv_wait_unlock(cv, mtx);
	for (;;) {
	    vaddr = atomic_readandclear_ptr(offp);
	    if (vaddr != 0) {

		/* XXX FENCES */
		page = worker->work_page;

		if (vaddr == SLS_POISON) {
		    destroy_desc(desc);
		    break; 
		}

		error = fd_write(&vaddr, sizeof(vm_offset_t), desc);
		if (error != 0) {
		    printf("Error: writing vm_map_entry_info failed\n");
		    continue;
		}

		/* Never fails on amd64, check is here for futureproofing */
		vaddr = userpage_map(page->phys_addr);
		if ((void *) vaddr == NULL) {
		    printf("Mapping page failed\n");
		    continue;
		}

		error = fd_write((void*) vaddr, PAGE_SIZE, desc);
		if (error != 0) {
		    printf("Error: write failed with %d\n", error);
		    continue;
		}

		userpage_unmap(vaddr);
	    }
	}
	printf("Worker %ld done with writing\n", id);
    }

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

	cv_init(&worker->work_cv, "slsworkercv");
	mtx_init(&worker->work_mtx, "slsworkedmtx", NULL, MTX_DEF);
	worker->work_offset = 0;
	worker->work_id = i;


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
    /* 
     * XXX Implement proper locking to eliminate
     * races between loads/unloads and dumps/restores
     */

    /*
     * XXX destroy threads somehow
     */
}

