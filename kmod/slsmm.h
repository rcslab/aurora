#ifndef __SLSMM_H__
#define __SLSMM_H__

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/ioccom.h>

#define VM_MAP_ENTRY_HEADER_OFFSET1 sizeof(vm_offset_t)
#define VM_MAP_ENTRY_HEADER_SIZE    VM_MAP_ENTRY_HEADER_OFFSET1+sizeof(vm_offset_t)

#define VM_OBJECT_HEADER_SIZE    sizeof(int)

#define VM_PAGE_HEADER_OFFSET1  sizeof(vm_paddr_t)
#define VM_PAGE_HEADER_OFFSET2  VM_PAGE_HEADER_OFFSET1+sizeof(vm_pindex_t)
#define VM_PAGE_HEADER_SIZE     VM_PAGE_HEADER_OFFSET2+sizeof(size_t)

#define SLSMM_DUMP _IOW('t', 1, int)

struct vm_map_entry_info {
    vm_offset_t start;
    vm_offset_t end;
};

struct vm_object_info {
    int resident_page_count;
};

struct vm_page_info {
    vm_paddr_t  phys_addr;
    vm_pindex_t pindex;
    size_t      pagesize;
};

#endif
