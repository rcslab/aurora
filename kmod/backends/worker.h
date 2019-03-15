#ifndef _WORKER_H_
#define _WORKER_H_

#include "../hash.h"

/* 
 * XXX Only one worker for now, the
 * bookkeeping in nvdimm.c assumes
 * this
 */
#define WORKER_THREADS (1)

int sls_workers_init(void);
void sls_workers_destroy(void);

/* XXX Padding? */
struct sls_worker {
    long cnt;
    long work_id;
    vm_offset_t work_index;
    struct thread *work_td;

    struct page_tailq work_list;
    struct mtx work_mtx;
    struct cv work_cv;

};

extern struct sls_worker sls_workers[WORKER_THREADS];
extern int time_to_die;

#endif /* _WORKER_H_ */
