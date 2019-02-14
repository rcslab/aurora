#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

#include <sys/types.h>

#include <sys/ioccom.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/shm.h>

#include <machine/param.h>
#include <machine/pcb.h>
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
	uint64_t fs_base;
};

int proc_checkpoint(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos);
int proc_restore(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos);

#endif
