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
            printf("%lx\n", page->pindex);
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
vm_map_dump(vm_map_t vmmap, vm_offset_t start, vm_offset_t end, struct thread *td, int fd) {
    int error = 0;
    int dump_page_count = 0;
    vm_map_entry_t entry;
    vm_pindex_t sidx;
    vm_pindex_t eidx;
    vm_object_t object;
    vm_page_t page;

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
        };
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
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused, 
        int flags __unused, struct thread *td) {
    int error = 0;
    vm_map_t p_vmmap;
    vm_map_entry_t entry;
    struct dump_range_req *param;
    struct proc *p;

    switch (cmd) {
        case SLSMM_DUMP_RANGE:
            param = (struct dump_range_req*)data;
            printf("%d\n", param->pid);

            if (param->pid == -1) p = td->td_proc;
            else {
                error = pget(param->pid, PGET_WANTREAD, &p);
                if (error) break;
            }
            p_vmmap = &p->p_vmspace->vm_map;
            printf("%d %lx %lx\n", param->pid, (vm_offset_t)p, (vm_offset_t)td->td_proc);
            printf("%lx\n", (vm_offset_t)p_vmmap);

            entry = p_vmmap->header.next; 

            vm_map_dump(p_vmmap, param->start, param->end, td, param->fd);

            if (param->pid != -1) PRELE(p);

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
