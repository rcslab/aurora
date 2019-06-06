#ifndef _DUMP_H_
#define _DUMP_H_

#include <sys/mman.h>

#include <vm/vm_map.h>

#include "slsmm.h"
#include "sls_data.h"

#define HASH_MAX (4 * 1024)

struct dump_page {
	vm_ooffset_t vaddr;
	void *data;
	LIST_ENTRY(dump_page) next;
};

LIST_HEAD(page_list, dump_page);

struct sls_pagetable {
    struct page_list *pages;		
    u_long hashmask;		
};

struct sls_store_tgt {
    int type;
    union {
	int fd;
	struct sls_snapshot *slss;
    };
};

int sls_store(struct sls_process *slsp, int mode, int fd);
int sls_load_cpustate(struct proc_info *proc_info, struct thread_info **thread_infos, struct file *fp);
int sls_load_filedesc(struct filedesc_info *filedesc, struct file *fp);
int sls_load_memory(struct memckpt_info *memory, struct file *fp);
int sls_load_ptable(struct sls_pagetable *ptable, struct file *fp);

void addpage_noreplace(struct sls_pagetable *ptable, struct dump_page *dump_page);

#endif /* _DUMP_H_ */
