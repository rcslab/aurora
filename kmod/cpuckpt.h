#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

#include <sys/types.h>

#include <sys/conf.h>
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

struct proc;
struct reg;
struct fpreg;

struct proc_info {
	size_t nthreads;
	pid_t pid;
};

struct thread_info {
	struct reg regs;
	struct fpreg fpregs;
	lwpid_t tid;
};

int thread_checkpoint(struct proc *p, struct thread_info *thread_info);
int thread_restore(struct proc *p, struct thread_info *thread_info);

int proc_checkpoint(struct proc *p, struct proc_info *proc_info);
int proc_restore(struct proc *p, struct proc_info *proc_info);

#endif
