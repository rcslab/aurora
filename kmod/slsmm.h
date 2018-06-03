#ifndef _SLSMM_H_
#define _SLSMM_H_

#include <sys/types.h>
#include <sys/ioccom.h>
#include <vm/vm.h>

struct vmspace_info {
    vm_offset_t min;
    vm_offset_t max;
    caddr_t vm_taddr;
    caddr_t vm_daddr;
    caddr_t vm_maxsaddr;
    int entry_count;
};

struct vm_map_entry_info {
    vm_offset_t start;
    vm_offset_t end;
    vm_offset_t offset;
    vm_prot_t protection;
    vm_prot_t max_protection;
};

struct vm_object_info {
    int resident_page_count;
    int dump_page_count;
};

struct vm_page_info {
    vm_paddr_t  phys_addr;
    vm_pindex_t pindex;
    size_t      pagesize;
};

struct dump_param {
    vm_offset_t start;
    vm_offset_t end;
    int         fd;
    int         pid;
};

struct restore_param {
    int     fd;
    int     pid;
};

#define SLSMM_DUMP      _IOW('d', 1, struct dump_param)
#define SLSMM_RESTORE   _IOW('r', 1, struct restore_param)

#endif
