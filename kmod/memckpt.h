#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include "slsmm.h"

#include <sys/types.h>

#include <vm/vm.h>

struct vmspace;
struct thread;
struct proc;

int
shadow_object_list(struct vmspace *vms, vm_object_t **objects, 
        struct vm_map_entry_info **entries, size_t *size);
int
vm_objects_dump(struct vmspace *vms, vm_object_t *objects, 
        struct vm_map_entry_info *entries, size_t size, int fd);

int
vmspace_restore(struct proc *p, struct thread *td, int fd);

#endif
