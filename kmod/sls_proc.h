#ifndef _SLS_PROC_H_
#define _SLS_PROC_H_

#include <sys/types.h>

#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/shm.h>

#include <machine/param.h>
#include <machine/pcb.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include "sls_data.h"

int sls_proc_ckpt(struct proc *p, struct sbuf *sb);
int sls_proc_rest(struct proc *p, struct proc_info *proc_info);
int sls_thread_rest(struct proc *p, struct thread_info *thread_info);


#endif /* _SLS_PROC_H_ */
