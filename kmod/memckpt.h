#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include <sys/types.h>

#include "slsmm.h"

struct vmspace;
struct thread;
struct proc;

vm_offset_t userpage_map(vm_paddr_t phys_addr);
void userpage_unmap(vm_offset_t vaddr);

int vmspace_checkpoint(struct vmspace *vms, struct dump *dump, long mode);
int vmspace_dump(struct dump *dump, int fd, long mode);
int vmspace_restore(struct proc *p, struct dump *dump);

#endif
