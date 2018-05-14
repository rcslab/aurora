#include "slsmm.h"
#include "_slsmm.h"

#include <sys/conf.h>
#include <sys/module.h>         // moduledata_t
#include <sys/proc.h>           // struct proc
#include <sys/pcpu.h>           // curproc
#include <sys/queue.h>          // TAILQ
#include <sys/syscallsubr.h>    // kern_writev
#include <sys/uio.h>            // struct uio

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>     //vm_page
#include <vm/vm_radix.h> 
#include <vm/vm_extern.h>   //vmspace_alloc
#include <vm/uma.h>         //uma_alloc

#include <machine/vmparam.h>

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
write_to_fd(void* addr, size_t len, struct thread *td, int fd) {
    int error = 0;
    
    struct uio auio;
    struct iovec aiov;
    bzero(&auio, sizeof(auio));
    bzero(&aiov, sizeof(aiov));

    aiov.iov_base = (void*)addr;
    aiov.iov_len = len;
    
    auio.uio_iov = &aiov;
    auio.uio_offset = 0;
    auio.uio_segflg = UIO_SYSSPACE;
    auio.uio_rw = UIO_WRITE;
    auio.uio_iovcnt = 1;
    auio.uio_resid = len;
    auio.uio_td = td;

    error = kern_writev(td, fd, &auio); 

    return error;
}

static int
read_from_fd(void* addr, size_t len, struct thread *td, int fd) {
    int error = 0;

    struct uio auio;
    struct iovec aiov;

    bzero(&auio, sizeof(auio));
    bzero(&aiov, sizeof(aiov));

    aiov.iov_base = (void*)addr;
    aiov.iov_len = len;

    auio.uio_iov = &aiov;
    auio.uio_offset = 0;
    auio.uio_segflg = UIO_SYSSPACE;
    auio.uio_rw = UIO_READ;
    auio.uio_iovcnt = 1;
    auio.uio_resid = len;
    auio.uio_td = td;
    
    error = kern_readv(td, fd, &auio);

    return error;
}

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
    write_to_fd(&page_info, sizeof(struct vm_page_info), td, fd);

    return write_to_fd((void*)vaddr, pagesize, td, fd);
}

static int
count_dump_page(vm_object_t object, vm_pindex_t sidx, vm_pindex_t eidx) {
    int count = 0;
    vm_page_t page;

    if (eidx - sidx > object->resident_page_count) {
        TAILQ_FOREACH(page, &object->memq, listq) {
            if (page->pindex >= sidx && page->pindex < eidx) count ++;
        }
    } else {
        for (vm_pindex_t idx = sidx; idx < eidx; idx ++) {
            page = vm_radix_lookup(&object->rtree, idx);
            if (page != NULL) count ++;
        }
    }

    return count;
}

static int
vmspace_dump(struct vmspace *vms, vm_offset_t start, vm_offset_t end, struct thread *td, int fd) {
    int error = 0;
    int dump_page_count = 0;
    int entry_count = 0;
    vm_map_t vmmap = &vms->vm_map;
    vm_map_entry_t entry;
    vm_pindex_t sidx;
    vm_pindex_t eidx;
    vm_object_t object;
    vm_page_t page;

    for (entry = vmmap->header.next; entry != &vmmap->header; entry = entry->next)
        entry_count ++;
    struct vmspace_info vms_info = {
        .min = vmmap->min_offset,
        .max = vmmap->max_offset,
        .vm_taddr = vms->vm_taddr,
        .vm_daddr = vms->vm_daddr,
        .vm_maxsaddr = vms->vm_maxsaddr, 
        .entry_count = entry_count,
    };
    printf("%lx %lx\n", vms_info.min, vms_info.max);
    write_to_fd(&vms_info, sizeof(struct vmspace_info), td, fd);

    for (entry = vmmap->header.next; entry != &vmmap->header; entry = entry->next) {
        object = entry->object.vm_object;
        sidx = start < entry->start ? 0 : (start - entry->start) / PAGE_SIZE;
        eidx = end < entry->start + 1 - PAGE_SIZE ? 0 : 
            (end + PAGE_SIZE - entry->start - 1) / PAGE_SIZE;

        dump_page_count = count_dump_page(object, sidx, eidx); 
        if (dump_page_count == 0) continue;

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
        printf("%lx %lx\n", entry_info.start, entry_info.end);
        write_to_fd(&entry_info, sizeof(struct vm_map_entry_info), td, fd);

        // vm_object header
        struct vm_object_info object_info = {
            .resident_page_count = object->resident_page_count,
            .dump_page_count = dump_page_count,
        };
        write_to_fd(&object_info, sizeof(struct vm_object_info), td, fd);

        if (eidx - sidx > object->resident_page_count) {
            TAILQ_FOREACH(page, &object->memq, listq) {
                if (page->pindex >= sidx && page->pindex < eidx) {
                    error = vm_page_dump(page, td, fd);
                    if (error) break;
                }
            }
        } else {
            for (vm_pindex_t idx = sidx; idx < eidx; idx ++) {
                page = vm_radix_lookup(&object->rtree, idx);
                if (page != NULL) {
                    error = vm_page_dump(page, td, fd);
                    if (error) break;
                }
            }
        }

        // decrease the ref_count. collapse intermediate shadow object
        vm_object_deallocate(object);
    }

    return error;
}

static int
vmspace_restore(struct proc *p, struct thread *td, int fd) {
    int error = 0;
    struct vmspace *newvmspace;
    struct vmspace_info vms_info;
    vm_map_t newmap;
    struct vm_map_entry_info entry_info;
    struct vm_object_info object_info;
    struct vm_page_info page_info;
    vm_map_entry_t entry;

    error = read_from_fd(&vms_info, sizeof(struct vmspace_info), td, fd);
    newvmspace = vmspace_alloc(vms_info.min, vms_info.max, NULL);
    if (newvmspace == NULL) return -1;
    newmap = &newvmspace->vm_map;
    newvmspace->vm_taddr = vms_info.vm_taddr;
    newvmspace->vm_daddr = vms_info.vm_daddr;
    newvmspace->vm_maxsaddr = vms_info.vm_maxsaddr;

    for (int i = 0; i < vms_info.entry_count; i ++) {
        error = read_from_fd(&entry_info, sizeof(struct vm_map_entry_info), td, fd);
        error = read_from_fd(&object_info, sizeof(struct vm_object_info), td, fd);
        printf("%lx %lx\n", entry_info.start, entry_info.end);

        //object = vm_object_allocate(OBJT_DEFAULT, atop(entry_info.end - entry_info.start));
        vm_map_lock(newmap);
        error = vm_map_insert(newmap, NULL, entry_info.offset, entry_info.start,
                entry_info.end, entry_info.protection, entry_info.max_protection, 0);
        vm_map_unlock(newmap);

        char tmp[4096];
        for (int i = 0; i < object_info.dump_page_count; i ++) {
            error = read_from_fd(&page_info, sizeof(struct vm_page_info), td, fd);
            error = read_from_fd(tmp, page_info.pagesize, td, fd);
        }

        if (error) break;
    } 

    printf("check\n");
    for (entry = newmap->header.next; entry != &newmap->header; entry = entry->next) {
        printf("%lx %lx\n", entry->start, entry->end);
        printf("%d\n", entry->object.vm_object->resident_page_count);
    }

    return error;
}

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused, 
        int flags __unused, struct thread *td) {
    int error = 0;
    struct dump_range_req *param;
    struct restore_req *restore_param;
    struct proc *p;

    switch (cmd) {
        case SLSMM_DUMP_RANGE:
            param = (struct dump_range_req*)data;

            if (param->pid == -1) p = td->td_proc;
            else {
                error = pget(param->pid, PGET_WANTREAD, &p);
                if (error) break;
            }

            error = vmspace_dump(p->p_vmspace, param->start, param->end, td, param->fd);

            if (param->pid != -1) PRELE(p);

            break;

        case SLSMM_RESTORE:
            printf("RESTORE\n");
            restore_param = (struct restore_req*)data;

            if (restore_param->pid == -1) p = td->td_proc;
            else {
                error = pget(restore_param->pid, PGET_WANTREAD, &p);
                if (error) break;
            }
            
            vmspace_restore(p, td, restore_param->fd);

            if (restore_param->pid != -1) PRELE(p);

            break;
    }

    return error;
}

static struct cdevsw slsmm_cdevsw = {
    .d_version = D_VERSION,
    .d_ioctl = slsmm_ioctl,
};
static struct cdev *slsmm_dev;

static int
SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
    int error = 0;
    switch (inEvent) {
        case MOD_LOAD:
            uprintf("Loaded\n");
            slsmm_dev = make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "slsmm");
            break;
        case MOD_UNLOAD:
            uprintf("Unloaded\n");
            destroy_dev(slsmm_dev);
            break;
        default:
            error = EOPNOTSUPP;
            break;
    }
    return error;
}

static moduledata_t moduleData = {
    "slsmm",
    SLSMMHandler,
    NULL
};


DECLARE_MODULE(slsmm_kmod, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE); 
