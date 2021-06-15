#ifndef _SLS_PROC_H_
#define _SLS_PROC_H_

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/sbuf.h>
#include <sys/shm.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#include <machine/param.h>
#include <machine/pcb.h>
#include <machine/reg.h>

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_kv.h"

int slsproc_checkpoint(struct proc *p, struct sbuf *sb, slsset *procset,
    struct slsckpt_data *sckpt_data);
int slsproc_restore(struct proc *p, uint64_t daemon, char **bufp,
    size_t *buflenp, struct slsrest_data *restdata);

#endif /* _SLS_PROC_H_ */
