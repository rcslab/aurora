#ifndef __SLSMM_H__
#define __SLSMM_H__

#include <sys/param.h>
#include <sys/malloc.h>
#include <sys/ioccom.h>

struct vm_map_entry_info {
    vm_offset_t start;
    vm_offset_t end;
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

struct dump_range_req {
    vm_offset_t start;
    vm_offset_t end;
    int fd;
    int pid;
};

#define SLSMM_DUMP          _IOW('d', 1, int)
#define SLSMM_DUMP_RANGE    _IOW('d', 2, struct dump_range_req)

#endif
