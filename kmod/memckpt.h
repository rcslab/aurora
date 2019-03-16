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

#define SLS_OBJECT_INFO_MAGIC 0x7aaa7303
struct vm_object_info {
	vm_pindex_t size;
    
	enum obj_type type;

	/* Used for mmap'd pages */
	size_t filename_len;
	char *filename;

	vm_offset_t id;

	/* Used for objects that are shadows of others */
	vm_offset_t backer;
	vm_ooffset_t backer_off;

	/* XXX Bookkeeping for swapped out pages? */
	int magic;
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

	struct vm_object_info *obj_info;
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

int vmspace_checkpoint(struct proc *p, struct memckpt_info *dump, 
		vm_object_t *objects, long mode);
int vmspace_restore(struct proc *p, struct memckpt_info *dump);

int vmspace_compact(struct proc *p);

/* 
 * XXX HACK. Properly implement delta dumps, as discussed. 
 *   Mixing full and delta dumps is not advised.
 */

#define SLS_MAX_PID 65536

extern int pid_checkpointed[SLS_MAX_PID];
extern vm_object_t pid_shadows[SLS_MAX_PID];

#endif
