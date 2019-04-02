#ifndef _DUMP_H_
#define _DUMP_H_

#include <sys/mman.h>

#include <vm/vm_map.h>

#include "slsmm.h"
#include "sls_data.h"
#include "sls_snapshot.h"

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
	struct sls_snapshot *slss;
    };
};

struct sls_snapshot *load_dump(int fd);
int store_dump(struct sls_snapshot *slss, int mode, struct vmspace *vm, int fd);
int store_pages(struct vmspace *vm, struct sls_store_tgt tgt, int mode);

int dump_clone(struct dump *dst, struct dump *src);
int copy_dump_pages(struct dump *dst, struct dump *src);

struct dump *alloc_dump(void);
void free_dump(struct dump *dump);

#endif /* _DUMP_H_ */
