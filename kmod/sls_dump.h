#ifndef _DUMP_H_
#define _DUMP_H_

#include <sys/mman.h>

#include "slsmm.h"
#include "sls_data.h"
#include "sls_process.h"

#define SLS_DUMP_MAGIC 0x736c7525
struct dump {
	struct proc_info proc;
	struct thread_info *threads;
	struct memckpt_info memory;
	struct filedesc_info filedesc;
	int magic;
};

struct sls_store_tgt {
    int type;
    union {
	int fd;
	struct sls_process *slsp;
    };
};

struct sls_process *load_dump(int fd);
int store_dump(struct sls_process *slsp, int mode, vm_object_t *objects, int fd);
int store_pages(struct vm_map_entry_info *entries, vm_object_t *objects, 
	    	size_t numentries, struct sls_store_tgt tgt, int mode);

int dump_clone(struct dump *dst, struct dump *src);
int copy_dump_pages(struct dump *dst, struct dump *src);

struct dump *alloc_dump(void);
void free_dump(struct dump *dump);

#endif /* _DUMP_H_ */
