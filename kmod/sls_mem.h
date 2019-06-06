#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include <sys/types.h>

#include <sys/sbuf.h>

#include "sls.h"
#include "sls_data.h"
#include "sls_dump.h"

/* XXX Move elsewhere, it's being used by multiple files */
#define IDX_TO_VADDR(idx, entry_start, entry_offset) \
	(IDX_TO_OFF(idx) + entry_start - entry_offset)
#define VADDR_TO_IDX(vaddr, entry_start, entry_offset) \
	(OFF_TO_IDX(vaddr - entry_start + entry_offset))

#define SLS_VMSPACE_INFO_MAGIC 0x736c7303

vm_offset_t userpage_map(vm_paddr_t phys_addr, size_t order);
void userpage_unmap(vm_offset_t vaddr);

int vmspace_ckpt(struct proc *p, struct sbuf *sb, long mode);
int vmspace_rest(struct proc *p, struct memckpt_info memckpt, struct sls_pagetable ptable);

#endif
