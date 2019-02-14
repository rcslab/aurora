#ifndef _DUMP_H_
#define _DUMP_H_

#include "cpuckpt.h"
#include "fd.h"
#include "fileio.h"
#include "memckpt.h"
#include "slsmm.h"

#define SLS_DUMP_MAGIC 0x736c7525
struct dump {
	struct proc_info proc;
	struct thread_info *threads;
	struct memckpt_info memory;
	struct filedesc_info filedesc;
    int magic;
};


int load_dump(struct dump *dump, struct sls_desc desc);
int store_dump(struct dump *dump, vm_object_t *objects,
                    long mode, struct sls_desc desc);

struct dump *compose_dump(struct sls_desc *descs, int ndescs);

int dump_clone(struct dump *dst, struct dump *src);
int copy_dump_pages(struct dump *dst, struct dump *src);

struct dump *alloc_dump(void);
void free_dump(struct dump *dump);

#endif /* _DUMP_H_ */
