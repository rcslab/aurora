#ifndef _MEMCKPT_H_
#define _MEMCKPT_H_

#include <sys/types.h>

#include "memckpt.h"

/*
 * XXX Small function or define for these two? I went with 
 * define because the operation is simple enough, and
 * so that we are free to use an uppercase name that 
 * denotes a simple calculation.
 */
#define IDX_TO_VADDR(idx, entry_start, entry_offset) \
	(IDX_TO_OFF(idx) + entry_start - entry_offset)
#define VADDR_TO_IDX(vaddr, entry_start, entry_offset) \
	(OFF_TO_IDX(vaddr - entry_start + entry_offset))

#define SLS_VMSPACE_INFO_MAGIC 0x736c7303
/* State of the vmspace, but also of its vm_map */
struct vmspace_info {
	int magic;
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

#define SLS_ENTRY_INFO_MAGIC 0x736c7304
/* State of a vm_map_entry and its backing object */
struct vm_map_entry_info {
	int magic;
	/* State of the map entry */
	vm_offset_t start;
	vm_offset_t end;
	vm_ooffset_t offset;
	vm_eflags_t eflags;
	vm_prot_t protection;
	vm_prot_t max_protection;

	/* State of the object*/
	vm_pindex_t size;
    int resident_page_count;
	enum obj_type type;

	/* Used for mmap'd pages */
	size_t filename_len;
	size_t file_offset;
	char *filename;

	/* Used for object that are shadows of others */
	boolean_t is_shadow;
	vm_ooffset_t backing_offset;
	/* 
	 * XXX Wow that's inelegant, but it's also as simple as it goes.
	 * We keep the address of the vm_object at checkpoint time, so that
	 * we know which object to create the shadow from at restore time.
	 *
	 * Also, what's the right type to store addresses? void * implies
	 * we are going to use it as a pointer, vm_offset_t is not used 
	 * outside the VM subsystem.
	 */
	vm_object_t current_object;
	vm_object_t backing_object;
	



	/* XXX Bookkeeping for swapped out pages? */
};

#define SLS_MEMCKPT_INFO_MAGIC 0x736c730a
struct memckpt_info {
	/* 
	 * XXX Actually use the magic, we aren't actually reading/
	 * writing it right now 
	 */
	int magic;
	struct vmspace_info vmspace;
	struct vm_map_entry_info *entries;
};

vm_offset_t userpage_map(vm_paddr_t phys_addr);
void userpage_unmap(vm_offset_t vaddr);

int vmspace_checkpoint(struct vmspace *vms, struct memckpt_info *dump, 
		vm_object_t *objects, long mode);
int vmspace_restore(struct proc *p, struct memckpt_info *dump);

#endif
