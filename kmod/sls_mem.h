#ifndef _SLS_MEM_H_
#define _SLS_MEM_H_

#include <sys/types.h>

#include <sys/sbuf.h>

#include "sls.h"
#include "sls_data.h"
#include "sls_dump.h"
#include "sls_load.h"
#include "sls_objtable.h"

/* XXX Move elsewhere, it's being used by multiple files */
#define IDX_TO_VADDR(idx, entry_start, entry_offset) \
	(IDX_TO_OFF(idx) + entry_start - entry_offset)
#define VADDR_TO_IDX(vaddr, entry_start, entry_offset) \
	(OFF_TO_IDX(vaddr - entry_start + entry_offset))

#define SLS_VMSPACE_INFO_MAGIC 0x736c7303

int sls_vmspace_ckpt(struct proc *p, struct sbuf *sb, long mode);
int sls_vmspace_rest(struct proc *p, struct memckpt_info memckpt);
int sls_vmobject_rest(struct vm_object_info *info, struct sls_objtable *objtable);
int sls_vmentry_rest(struct vm_map *map, struct vm_map_entry_info *entry, struct sls_objtable *objtable);
void sls_data_rest(struct sls_pagetable ptable, struct vm_map *map, struct vm_map_entry *entry);

#endif /* _SLS_MEM_H_ */
