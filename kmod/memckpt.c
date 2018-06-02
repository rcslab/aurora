#include "memckpt.h"
#include "slsmm.h"
#include "fileio.h"

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <machine/param.h>

static int
vm_page_dump(vm_page_t page, struct thread *td, int fd) {
    size_t pagesize = pagesizes[page->psind];
    //TODO: unmap
    vm_offset_t vaddr = pmap_map(NULL, page->phys_addr, 
            page->phys_addr+pagesize, VM_PROT_READ);

    struct vm_page_info page_info = {
        .phys_addr  = page->phys_addr,
        .pindex     = page->pindex,
        .pagesize   = pagesize,
    };
    fd_write(&page_info, sizeof(struct vm_page_info), fd);

    return fd_write((void*)vaddr, pagesize, fd);
}

static int
count_inrange_page(vm_map_entry_t entry, vm_offset_t start, vm_offset_t end) {
    int count = 0;
    vm_page_t page;

    TAILQ_FOREACH(page, &entry->object.vm_object->memq, listq) {
        vm_offset_t vaddr = entry->start + page->pindex * PAGE_SIZE;
        if (vaddr >= start && vaddr < end) count ++;
    }

    return count;
}

int
vmspace_dump(struct vmspace *vms, vm_offset_t start, vm_offset_t end, 
        struct thread *td, int fd) {
    int error = 0;
    int dump_page_count = 0;
    int entry_count = 0;
    vm_map_t vmmap = &vms->vm_map;
    vm_map_entry_t entry;
    vm_object_t object;
    vm_page_t page;

    for (entry = vmmap->header.next; entry != &vmmap->header; entry = entry->next)
        if (count_inrange_page(entry, start ,end) >= 0) entry_count ++;

    struct vmspace_info vms_info = {
        .min = vmmap->min_offset,
        .max = vmmap->max_offset,
        .vm_taddr = vms->vm_taddr,
        .vm_daddr = vms->vm_daddr,
        .vm_maxsaddr = vms->vm_maxsaddr, 
        .entry_count = entry_count,
    };
    printf("entry count %d\n", vms_info.entry_count);
    fd_write(&vms_info, sizeof(struct vmspace_info), fd);

    for (entry = vmmap->header.next; entry != &vmmap->header; entry = entry->next) {
        object = entry->object.vm_object;

        dump_page_count = count_inrange_page(entry, start, end); 
        printf("%lx %lx %d\n", entry->start, entry->end, dump_page_count);
        //if (dump_page_count == 0) continue;

        vm_object_reference(entry->object.vm_object);
        vm_object_shadow(&entry->object.vm_object, &entry->offset, 
                entry->end-entry->start);
        pmap_remove(vmmap->pmap, entry->start, entry->end);

        // vm_map_entry header
        struct vm_map_entry_info entry_info = {
            .start  = entry->start,
            .end    = entry->end,
            .offset = entry->offset,
        };
        //printf("%lx %lx\n", entry_info.start, entry_info.end);
        fd_write(&entry_info, sizeof(struct vm_map_entry_info), fd);

        // vm_object header
        struct vm_object_info object_info = {
            .resident_page_count = object->resident_page_count,
            .dump_page_count = dump_page_count,
        };
        fd_write(&object_info, sizeof(struct vm_object_info), fd);

        TAILQ_FOREACH(page, &object->memq, listq) {
            vm_offset_t vaddr = entry->start + page->pindex * PAGE_SIZE;
            if (vaddr >= start && vaddr < end) {
                error = vm_page_dump(page, td, fd);
                if (error) break;
            }
        }

        // decrease the ref_count. collapse intermediate shadow object
        vm_object_deallocate(object);
    }

    return error;
}

int
vmspace_restore(struct proc *p, struct thread *td, int fd) {
    int error = 0;
    struct vmspace *newvmspace;
    struct vmspace_info vms_info;
    vm_map_t newmap;
    struct vm_map_entry_info entry_info;
    struct vm_object_info object_info;
    struct vm_page_info page_info;
    vm_object_t object;
    vm_map_entry_t entry;
    vm_page_t page;
    vm_offset_t vaddr;

    error = fd_read(&vms_info, sizeof(struct vmspace_info), fd);
    newvmspace = vmspace_alloc(vms_info.min, vms_info.max, NULL);
    if (newvmspace == NULL) return -1;
    newmap = &newvmspace->vm_map;
    newvmspace->vm_taddr = vms_info.vm_taddr;
    newvmspace->vm_daddr = vms_info.vm_daddr;
    newvmspace->vm_maxsaddr = vms_info.vm_maxsaddr;

    printf("entry count %d\n", vms_info.entry_count);
    for (int i = 0; i < vms_info.entry_count; i ++) {
        error = fd_read(&entry_info, sizeof(struct vm_map_entry_info), fd);
        error = fd_read(&object_info, sizeof(struct vm_object_info), fd);
        printf("%lx %lx\n", entry_info.start, entry_info.end);

        object = vm_object_allocate(OBJT_DEFAULT, 
                atop(entry_info.end - entry_info.start));

        VM_OBJECT_WLOCK(object);
        for (int i = 0; i < object_info.dump_page_count; i ++) {
            error = fd_read(&page_info, sizeof(struct vm_page_info), fd);

            page = vm_page_alloc(object, page_info.pindex, VM_ALLOC_NORMAL);

            vaddr = pmap_map(NULL, page->phys_addr, 
                    page->phys_addr+PAGE_SIZE, VM_PROT_READ);

            error = fd_read((void*)vaddr, page_info.pagesize, fd);
        }
        VM_OBJECT_WUNLOCK(object);

        vm_object_reference(object);
        vm_map_lock(newmap);
        error = vm_map_insert(newmap, object, entry_info.offset, entry_info.start,
                entry_info.end, entry_info.protection, entry_info.max_protection, 0);
        vm_map_unlock(newmap);
        if (error) break;
    } 


    printf("check\n");
    entry = newmap->header.next;
    for (entry = newmap->header.next; entry != &newmap->header; entry = entry->next) {
        object = entry->object.vm_object;
        printf("%lx %lx %d\n", entry->start, entry->end, object->resident_page_count);
    }

    p->p_vmspace = newvmspace;

    printf("return\n");
    return error;
}
