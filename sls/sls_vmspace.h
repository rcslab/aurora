#ifndef _SLS_MEM_H_
#define _SLS_MEM_H_

#include <sys/types.h>
#include <sys/sbuf.h>

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_load.h"
#include "sls_table.h"

#define SLS_VMSPACE_INFO_MAGIC 0x736c7303

int slsckpt_vmspace(
    struct vmspace *vm, struct sbuf *sb, struct slsckpt_data *sckpt_data);

int slsrest_vmspace(struct proc *p, struct slsvmspace *slsvmspace,
    struct shmmap_state *shmstate);
int slsrest_vmentry(struct vm_map *map, struct slsvmentry *slsvmentry,
    struct slsrest_data *restdata);
#endif /* _SLS_MEM_H_ */
