#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

#include <sys/types.h>

#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/shm.h>

#include <machine/param.h>
#include <machine/pcb.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include "sls_data.h"

int proc_checkpoint(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos);
int proc_restore(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos);

#endif
