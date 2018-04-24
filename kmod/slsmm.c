#include "slsmm.h"

#include <sys/conf.h>
#include <sys/module.h> // moduledata_t
#include <sys/proc.h>   // struct proc
#include <sys/pcpu.h>   // curproc

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data __unused, 
        int flags __unused, struct thread *td) {
    int error = 0;

    switch (cmd) {
        case SLSMM_VMSPACE:
            printf("SLSMM_VMSPACE\n");
            printf("thread %u ", (unsigned int)td);
            printf("curthread %u\n", (unsigned int)curthread);
            printf("tdproc %u curproc %u ", (unsigned int)td->td_proc, 
                    (unsigned int)curproc);

            vm_map_t p_vmmap = &td->td_proc->p_vmspace->vm_map;
            vm_map_entry_t entry = p_vmmap->header.next;

            for (int i = 0; entry != &p_vmmap->header; i ++, entry = entry->next) {
                printf("entry %d\n", i);
                vm_object_t object = entry->object.vm_object;
                printf("before %u shadow %d\n", (unsigned int)object,
                        object->shadow_count);
                vm_object_reference(object);
                vm_object_shadow(&entry->object.vm_object, &entry->offset, 
                            entry->end-entry->start);
                // on vm_map.c line 3437, it is deallocating the old object,
                // I do not understand why it is doing so
                //vm_object_deallocate(object);
                object = entry->object.vm_object;
                printf("after %u shadow %d\n", (unsigned int)object,
                        object->shadow_count);
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
