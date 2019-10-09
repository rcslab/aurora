#ifndef _SLS_MEM_H_
#define _SLS_MEM_H_

#include <sys/types.h>

#include <sys/sbuf.h>

#include "sls.h"
#include "slskv.h"
#include "slstable.h"
#include "sls_data.h"
#include "sls_load.h"

#define SLS_VMSPACE_INFO_MAGIC 0x736c7303

int sls_vmspace_ckpt(struct proc *p, struct sbuf *sb, long mode);

int sls_vmspace_rest(struct proc *p, struct memckpt_info memckpt);
int sls_vmobject_rest(struct vm_object_info *info, struct slskv_table *objtable, struct slsdata *slsdata);
int sls_vmentry_rest(struct vm_map *map, struct vm_map_entry_info *entry, struct slskv_table *objtable);


void sls_shadow(vm_object_t shadow, vm_object_t source, vm_ooffset_t offset);
#endif /* _SLS_MEM_H_ */
