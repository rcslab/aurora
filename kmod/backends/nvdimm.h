#ifndef _NVDIMM_H_
#define _NVDIMM_H_

#include <sys/param.h>

#include <sys/bio.h>

#include <vm/vm.h>
#include <vm/uma.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include "../memckpt.h"

/* copied from the nvdimm driver */
enum SPA_mapping_type {
	SPA_TYPE_VOLATILE_MEMORY	= 0,
	SPA_TYPE_PERSISTENT_MEMORY	= 1,
	SPA_TYPE_CONTROL_REGION		= 2,
	SPA_TYPE_DATA_REGION		= 3,
	SPA_TYPE_VOLATILE_VIRTUAL_DISK	= 4,
	SPA_TYPE_VOLATILE_VIRTUAL_CD	= 5,
	SPA_TYPE_PERSISTENT_VIRTUAL_DISK= 6,
	SPA_TYPE_PERSISTENT_VIRTUAL_CD	= 7,
};

struct SPA_mapping {
	enum SPA_mapping_type	spa_type;
	int			spa_domain;
	int			spa_nfit_idx;
	uint64_t		spa_phys_base;
	uint64_t		spa_len;
	uint64_t		spa_efi_mem_flags;
	void			*spa_kva;
	struct cdev		*spa_dev;
	struct g_geom		*spa_g;
	struct g_provider	*spa_p;
	struct bio_queue_head	spa_g_queue;
	struct mtx		spa_g_mtx;
	struct mtx		spa_g_stat_mtx;
	struct devstat		*spa_g_devstat;
	struct proc		*spa_g_proc;
	struct vm_object	*spa_obj;
	bool			spa_g_proc_run;
	bool			spa_g_proc_exiting;
};

extern void *nvdimm;
extern size_t nvdimm_size;

#define NVDIMM_SLICE_SIZE (1024UL * 1024 * 1024 * 16)

inline void *
nvdimm_slice(int index)
{
    return (void *) ((vm_offset_t) nvdimm + index * NVDIMM_SLICE_SIZE);
}

#define NVDIMM_NAME ("/dev/nvdimm_spa1")

int nvdimm_open(void);
void nvdimm_close(void);
int nvdimm_read(void *addr, size_t len, vm_offset_t *offset);
int nvdimm_write(void *addr, size_t len, vm_offset_t *offset);
void nvdimm_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
		    size_t numentries, uintptr_t *addr, int mode);

struct sls_pagedata {
    vm_ooffset_t vaddr;
    uintptr_t dest;
    vm_page_t page;
};

#define PAGECHUNK_SIZE ((4096 / sizeof(struct sls_pagedata)) - 2)

struct sls_pagechunk {
    size_t size;
    struct sls_pagedata pagedata[PAGECHUNK_SIZE];
    LIST_ENTRY(sls_pagechunk) next;
};

LIST_HEAD(sls_chunkq, sls_pagechunk);



/* 
 * XXX Only one worker for now, the
 * bookkeeping in nvdimm.c assumes
 * this
 */
#define WORKER_THREADS (8)

int sls_workers_init(void);
void sls_workers_destroy(void);

struct sls_worker {
    char top_padding[128];

    long work_id;
    struct thread *work_td;
    int work_in_progress;

    struct sls_chunkq work_list;
    struct mtx work_mtx;
    struct cv work_cv;

    char bot_padding[128];
};

extern struct sls_worker sls_workers[WORKER_THREADS];
extern int time_to_die;

#endif /* _NVDIMM_H_ */
