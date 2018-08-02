#include "memckpt.h"
#include "_slsmm.h"
#include "slsmm.h"
#include "fileio.h"

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/shm.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <machine/param.h>

static void
print_shadow_list(vm_object_t object)
{
    vm_object_t obj;
    
    for (obj = object; obj != NULL; obj = obj->backing_object) {
        if (obj != object) printf("->");
        printf("%lx", (vm_offset_t)obj);
    }
    printf("\n");
}

int
shadow_object_list(struct vmspace *vms, vm_object_t **objects, 
        struct vm_map_entry_info **entries, size_t *size)
{
    int error = 0;
    vm_map_t vmmap = &vms->vm_map;
    vm_map_entry_t entry;

    *size = 0;
    *objects = malloc(vmmap->nentries*sizeof(vm_object_t), M_SLSMM, M_NOWAIT); 
    if (*objects == NULL) return ENOMEM;
    *entries = malloc(vmmap->nentries*sizeof(struct vm_map_entry_info), M_SLSMM, M_NOWAIT); 
    if (*entries == NULL) {
        if (*objects) free(*objects, M_SLSMM);
        return ENOMEM;
    }

    for (entry = vmmap->header.next; entry != &vmmap->header; entry = entry->next) {
        (*objects)[*size] = entry->object.vm_object;
        if ((*objects)[*size] == NULL) continue;
        
        (*entries)[*size].start = entry->start;
        (*entries)[*size].end = entry->end;
        (*entries)[*size].offset = entry->offset;
        *size += 1;

        vm_object_reference(entry->object.vm_object);
        vm_object_shadow(&entry->object.vm_object, &entry->offset,
                entry->end-entry->start);
        pmap_remove(vmmap->pmap, entry->start, entry->end);
    }

    return error;
}

int
vm_objects_dump(struct vmspace *vms, vm_object_t *objects, 
       struct vm_map_entry_info *entries, size_t size, int fd)
{
    int error = 0;
    vm_map_t vmmap = &vms->vm_map;
    struct vmspace_info vms_info = {
        .min = vmmap->min_offset,
        .max = vmmap->max_offset,
        .vm_swrss = vms->vm_swrss,
        .vm_tsize = vms->vm_tsize,
        .vm_dsize = vms->vm_dsize,
        .vm_ssize = vms->vm_ssize,
        .vm_taddr = vms->vm_taddr,
        .vm_daddr = vms->vm_daddr,
        .vm_maxsaddr = vms->vm_maxsaddr, 
        .entry_count = vmmap->nentries,
    };

    error = fd_write(&vms_info, sizeof(struct vmspace_info), fd);
    if (error) return error;

    printf("%lx\n", (vm_offset_t)entries);
    for (size_t i = 0; i < size; i ++) {
        vm_page_t page;
        printf("entry %zu %lx %lx %d\n", i, entries[i].start, entries[i].end,
                objects[i]->resident_page_count);

        error = fd_write(entries+i, sizeof(struct vm_map_entry_info), fd);
        if (error) break;

        struct vm_object_info object_info = {
            .resident_page_count = objects[i]->resident_page_count,
            .dump_page_count = objects[i]->resident_page_count,
        };
        error = fd_write(&object_info, sizeof(struct vm_object_info), fd);
        if (error) break;

        // dump page
        TAILQ_FOREACH(page, &objects[i]->memq, listq) {
            vm_offset_t vaddr = pmap_map(NULL, page->phys_addr, 
                    page->phys_addr+PAGE_SIZE, VM_PROT_READ);

            struct vm_page_info page_info = {
                .phys_addr  = page->phys_addr,
                .pindex     = page->pindex,
                .pagesize   = PAGE_SIZE,
            };
            error = fd_write(&page_info, sizeof(struct vm_page_info), fd);
            if (error) break;
            error = fd_write((void*)vaddr, PAGE_SIZE, fd);
            if (error) break;
        }

        vm_object_deallocate(objects[i]);

    }

    return error;
}

int
vmspace_restore(struct proc *p, struct thread *td, int fd) {
    int error = 0;
    struct vmspace_info vms_info;
    struct vmspace *vms;
    vm_map_t map;

    error = fd_read(&vms_info, sizeof(struct vmspace_info), fd);
    if (error) return error;

    vms = p->p_vmspace;
    map = &vms->vm_map;
    if (vms->vm_refcnt == 1 && vm_map_min(map) == vms_info.min && 
            vm_map_max(map) == vms_info.max) {
        shmexit(vms);
        pmap_remove_pages(vmspace_pmap(vms));
        vm_map_remove(map, vm_map_min(map), vm_map_max(map));
    } else {
        error = vmspace_exec(p, vms_info.min, vms_info.max);
        if (error) return error;
        vms = p->p_vmspace;
        map = &vms->vm_map;
    }

    printf("%lx %lx %lx %lx\n", vms->vm_swrss, vms->vm_tsize, 
            vms->vm_dsize, vms->vm_ssize);

    for (int i = 0; i < vms_info.entry_count; i ++) {
        struct vm_map_entry_info entry_info;
        struct vm_object_info object_info;
        vm_object_t object;

        error = fd_read(&entry_info, sizeof(struct vm_map_entry_info), fd);
        if (error) return error;
        error = fd_read(&object_info, sizeof(struct vm_object_info), fd);
        if (error) return error;

        object = vm_object_allocate(OBJT_DEFAULT, 
                atop(entry_info.end-entry_info.start));
        if (object == NULL) {
            printf("vm_object_allocate error\n");
            return (ENOMEM);
        }

        VM_OBJECT_WLOCK(object);
        for (int j = 0; j < object_info.dump_page_count; j ++) {
            struct vm_page_info page_info;
            vm_page_t page;
            vm_offset_t vaddr;

            error = fd_read(&page_info, sizeof(struct vm_page_info), fd);
            if (error) {
                printf("fd_read page_info error\n");
                VM_OBJECT_WUNLOCK(object);
                return error;
            }

            page = vm_page_alloc(object, page_info.pindex, VM_ALLOC_NORMAL);
            if (page == NULL) {
                printf("vm_page_alloc error\n");
                return (ENOMEM);
            }
            vaddr = pmap_map(NULL, page->phys_addr, page->phys_addr+PAGE_SIZE,
                    VM_PROT_WRITE);
            printf("%lx\n", vaddr);
            error = fd_read((void*)vaddr, page_info.pagesize, fd); 

            if (error) {
                printf("fd_read data error\n");
                VM_OBJECT_WUNLOCK(object);
                return error;
            }
        }
        VM_OBJECT_WUNLOCK(object);

        vm_object_reference(object);
        vm_map_lock(map);
        error = vm_map_insert(map, object, entry_info.offset, entry_info.start,
                entry_info.end, entry_info.protection, 
                entry_info.max_protection, MAP_COPY_ON_WRITE | MAP_PREFAULT);
        vm_map_unlock(map);

        if (error) {
            printf("vm_map_insert error\n");
            return error;
        }
    }

    vms->vm_swrss = vms_info.vm_swrss;
    vms->vm_tsize = vms_info.vm_tsize;
    vms->vm_dsize = vms_info.vm_dsize;
    vms->vm_ssize = vms_info.vm_ssize;
    vms->vm_taddr = vms_info.vm_taddr;
    vms->vm_daddr = vms_info.vm_daddr;
    vms->vm_maxsaddr = vms_info.vm_maxsaddr;

    printf("%lx %lx %lx %lx\n", vms->vm_swrss, vms->vm_tsize, 
            vms->vm_dsize, vms->vm_ssize);

    vm_map_entry_t entry;
    for (entry = map->header.next; entry != &map->header; entry = entry->next) {
        vm_object_t object = entry->object.vm_object;
        printf("%lx %lx %d\n", entry->start, entry->end, object->resident_page_count);
    }

    return error;
}

