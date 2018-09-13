#ifndef _SLSMM_H_
#define _SLSMM_H_

#include <sys/types.h>

#include <sys/ioccom.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/shm.h>

#include <machine/param.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#define SLS_PROC_INFO_MAGIC 0x736c7301
struct proc_info {
	int magic;
	size_t nthreads;
	pid_t pid;
	/* All fields valid except for the mutex */
	struct sigacts sigacts;
};

#define SLS_THREAD_INFO_MAGIC 0x736c7302
struct thread_info {
	int magic;
	struct reg regs;
	struct fpreg fpregs;
	lwpid_t tid;
	sigset_t sigmask;
	sigset_t oldsigmask;
};

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
	/* XXX: Obey inheritance values */
	/* vm_inherit_t inheritance; */
	/* State of the object*/
	vm_pindex_t size;
	/* XXX Bookkeeping for swapped out pages? */
};

struct dump {
	struct proc_info proc;
	struct thread_info *threads;
	struct vmspace_info vmspace;
	struct vm_map_entry_info *entries;
	vm_object_t *objects;
};

struct dump_param {
	int	fd;
	int	pid;
};

struct restore_param {
	int pid;
	int nfds;
	int *fds;
};

int load_dumps(struct dump *dump, int nfds, int *fds);
int load_dump(struct dump *dump, int fd);
struct dump *alloc_dump(void);
void free_dump(struct dump *dump);

#define FULL_DUMP		_IOW('d', 1, struct dump_param)
#define DELTA_DUMP		_IOW('d', 2, struct dump_param)
#define SLSMM_RESTORE	_IOW('r', 1, struct restore_param)

#define SLSMM_CKPT_FULL		0
#define SLSMM_CKPT_DELTA	1

#endif
