#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include <sys/types.h>

#include <sys/conf.h>
#include <sys/ioccom.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

#include <machine/param.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "slsmm.h"

struct vmspace;
struct thread;
struct proc;


/* State of the vmspace, but also of its vm_map */
struct vmspace_info {
    /* State of the vmspace object */
    segsz_t vm_swrss;
    segsz_t vm_tsize;
    segsz_t vm_dsize;
    segsz_t vm_ssize;
    caddr_t vm_taddr;
    caddr_t vm_daddr;
    caddr_t vm_maxsaddr;
    int nentries;
    /*  
     * Needed to know how many index-page pairs
     * to read from the file 
     */
};

/* State of a vm_map_entry and its backing object */
struct vm_map_entry_info {
    /* State of the map entry */
    vm_offset_t start;
    vm_offset_t end;
    vm_ooffset_t offset;
    vm_eflags_t eflags;
    vm_prot_t protection;
    vm_prot_t max_protection;
    /* XXX: Obey inheritance values */
    /* vm_inherit_t inheritance; */
    /* State of the object*/
    vm_pindex_t size;
    /* XXX Bookkeeping for swapped out pages? */
};


int vmspace_checkpoint(struct vmspace *vmspace, int fd);
int vmspace_restore(struct proc *p, int fd);

#endif
