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

#define SLS_DUMP_MAGIC 0x736c7525
struct dump {
	struct proc_info proc;
	struct thread_info *threads;
	struct memckpt_info memory;
	struct filedesc_info filedesc;
	struct page_list *pages;		
	u_long hashmask;		
	int magic;
};

struct sls_store_tgt {
    int type;
    union {
	int fd;
	struct sls_snapshot *slss;
    };
};

struct dump *sls_load(int fd);
int sls_store(struct dump *dump, int mode, struct vmspace *vm, int fd);

int dump_clone(struct dump *dst, struct dump *src);
int copy_dump_pages(struct dump *dst, struct dump *src);

struct dump *alloc_dump(void);
void free_dump(struct dump *dump);

void addpage_noreplace(struct dump *dump, struct dump_page *dump_page);

#endif /* _DUMP_H_ */
