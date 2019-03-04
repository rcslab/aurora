#ifndef _WORKER_H_
#define _WORKER_H_

#define WORKER_THREADS (1)

int sls_workers_init(void);
void sls_workers_destroy(void);

/* XXX Padding? */
struct sls_worker {
    long work_id;
    vm_offset_t work_offset;
    struct sls_desc work_desc;
    vm_page_t work_page;
    struct thread *work_td;

    struct mtx work_mtx;
    struct cv work_cv;

};

extern struct sls_worker sls_workers[WORKER_THREADS];

#endif /* _WORKER_H_ */
