#ifndef _DUMP_H_
#define _DUMP_H_

#include "cpuckpt.h"
#include "dump.h"
#include "memckpt.h"
#include "slsmm.h"

struct dump {
	struct proc_info proc;
	struct thread_info *threads;
	struct memckpt_info memory;
};

int vmspace_dump(struct dump *dump, vm_object_t *objects, struct dump_param *param, long mode);

int load_dumps(struct dump *dump, int nfds, int *fds);
int load_dump(struct dump *dump, int fd, int fd_type);

struct dump *compose_dump(struct restore_param *param);

int dump_clone(struct dump *dst, struct dump *src);
int copy_dump_pages(struct dump *dst, struct dump *src);

struct dump *alloc_dump(void);
void free_dump(struct dump *dump);

#endif /* _DUMP_H_ */
