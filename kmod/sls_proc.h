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
#include "sls_kv.h"
#include "sls_internal.h"

int slsckpt_proc(struct proc *p, struct sbuf *sb);
int slsrest_proc(struct proc *p, struct slsproc *slsproc, struct slsrest_data *restdata);
int slsrest_thread(struct proc *p, struct slsthread *slsthread);


#endif /* _SLS_PROC_H_ */
