#ifndef _SLS_MEM_H_
#define _SLS_MEM_H_

#include <sys/types.h>

#include <sys/sbuf.h>

#include <sls_data.h>

#include "sls_internal.h"
#include "sls_data.h"
#include "sls_kv.h"
#include "sls_table.h"
#include "sls_load.h"

#define SLS_VMSPACE_INFO_MAGIC 0x736c7303

int slsckpt_vmspace(struct proc *p, struct sbuf *sb, long mode);

void sls_shadow(vm_object_t shadow, vm_object_t source, vm_ooffset_t offset);

/* SYSV shared memory state (taken from the kernel) */
struct shmmap_state {
    vm_offset_t va;
    int shmid;
};

#endif /* _SLS_MEM_H_ */
