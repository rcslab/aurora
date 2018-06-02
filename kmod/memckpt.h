#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include <sys/types.h>

struct vmspace;
struct thread;
struct proc;

int
vmspace_dump(struct vmspace *vms, vm_offset_t start, vm_offset_t end, 
        struct thread *td, int fd);
int
vmspace_restore(struct proc *p, struct thread *td, int fd);

#endif
