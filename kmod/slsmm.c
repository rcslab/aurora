#include "slsmm.h"

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

#include <machine/vmparam.h>

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
write_page_to_fd(vm_page_t page, struct thread *td, int fd) {
    int error = 0;
    
    size_t pagesize = pagesizes[page->psind];
    //TODO: unmap
    vm_offset_t vaddr = pmap_map(NULL, page->phys_addr, 
            page->phys_addr+pagesize, VM_PROT_READ);

    struct uio auio;
    struct iovec aiov;
    bzero(&auio, sizeof(auio));
    bzero(&aiov, sizeof(aiov));

    aiov.iov_base = (void*)vaddr;
    aiov.iov_len = pagesize;
    
    auio.uio_iov = &aiov;
    auio.uio_offset = 0;
    auio.uio_segflg = UIO_SYSSPACE;
    auio.uio_rw = UIO_WRITE;
    auio.uio_iovcnt = 1;
    auio.uio_resid = pagesize;
    auio.uio_td = td;

    error = kern_writev(td, fd, &auio); 

    return error;
}

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused, 
        int flags __unused, struct thread *td) {
    int error = 0;
    int fd = 0;
    vm_map_t p_vmmap;
    vm_map_entry_t entry;

    switch (cmd) {
        case SLSMM_DUMP:
            fd = *(int*)data;
            printf("SLSMM_DUMP %d\n", fd);

            p_vmmap = &td->td_proc->p_vmspace->vm_map;
            entry = p_vmmap->header.next; 

            for (int i = 0; entry != &p_vmmap->header; i ++, entry = entry->next) {
                vm_object_t object  = entry->object.vm_object;

                vm_object_reference(object);
                vm_object_shadow(&entry->object.vm_object, &entry->offset, 
                        entry->end-entry->start);

                // dump original vm_object
                vm_page_t page;
                TAILQ_FOREACH(page, &object->memq, listq) {
                    error = write_page_to_fd(page, td, fd);
                    if (error) return error;
                }

                // on vm_map.c line 3437, it is deallocating the old object,
                // I do not understand why it is doing so
                //vm_object_deallocate(object);
                object = entry->object.vm_object;
                // the backing object should be the original object
                printf("backing_object %u\n", (unsigned int)object->backing_object);
                vm_object_reference(object);

            }

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
