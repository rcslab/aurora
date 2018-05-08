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
vm_map_entry_dump(vm_map_entry_t entry, vm_offset_t start, vm_offset_t end, 
        struct thread *td, int fd)
{
    int error = 0;
    int dump_page_count = 0;
    vm_page_t page;
    vm_pindex_t sindex = (start-entry->start)/PAGE_SIZE;
    vm_pindex_t eindex = (end-entry->start+PAGE_SIZE-1)/PAGE_SIZE;

    vm_object_t object = entry->object.vm_object;

    // count number of pages to dump
    for (vm_pindex_t idx = sindex; idx < eindex; idx ++) {
        page = vm_radix_lookup(&object->rtree, idx);
        if (page != NULL) dump_page_count ++;
    }
    if (dump_page_count == 0) return 0;

    vm_object_reference(entry->object.vm_object);
    vm_object_shadow(&entry->object.vm_object, &entry->offset, 
            entry->end-entry->start);
    pmap_remove(td->td_proc->p_vmspace->vm_map.pmap, entry->start, entry->end);

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

    // dump pages
    for (vm_pindex_t idx = sindex; idx < eindex; idx ++) {
        page = vm_radix_lookup(&object->rtree, idx);
        if (page == NULL) continue;

        error = vm_page_dump(page, td, fd);
        if (error) break;

        dump_page_count --;
        if (dump_page_count == 0) break;
    }

    // decrease the ref_count. collapse intermediate shadow object
    vm_object_deallocate(object);

    return error;
}

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused, 
        int flags __unused, struct thread *td) {
    int error = 0;
    int fd = 0;
    vm_map_t p_vmmap;
    vm_map_entry_t entry;
    struct dump_range_req *param;

    switch (cmd) {
        case SLSMM_DUMP:
            printf("SLSMM_DUMP\n");
            fd = *(int*)data;

            p_vmmap = &td->td_proc->p_vmspace->vm_map;
            entry = p_vmmap->header.next; 

            for (int i = 0; entry != &p_vmmap->header; i ++, entry = entry->next) 
                vm_map_entry_dump(entry, 0, (vm_offset_t)-1, td, fd);

            break;

        case SLSMM_DUMP_RANGE:
            param = (struct dump_range_req*)data;

            p_vmmap = &td->td_proc->p_vmspace->vm_map;
            entry = p_vmmap->header.next; 

            for (int i = 0; entry != &p_vmmap->header; i ++, entry = entry->next)
                vm_map_entry_dump(entry, param->start, param->end, td, param->fd);
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
