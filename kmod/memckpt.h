#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include <sys/types.h>

#include "slsmm.h"

struct vmspace;
struct thread;
struct proc;

int vmspace_checkpoint(struct vmspace *vmspace, int fd);
int vmspace_restore(struct proc *p, struct dump *dump);

int object_list_get(struct vmspace *vms, vm_object_t **objects,
		    struct vm_map_entry_info **entries);
int object_list_dump(struct vmspace *vmspace, vm_object_t *objects,
		     struct vm_map_entry_info *entries, int fd);

#endif
