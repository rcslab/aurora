#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

#include "slsmm.h"

#include <sys/types.h>

struct proc;
struct reg;
struct fpreg;

int thread_checkpoint(struct proc *p, struct thread_info *thread_info);
int thread_restore(struct proc *p, struct thread_info *thread_info);

int proc_checkpoint(struct proc *p, struct proc_info *proc_info);
int proc_restore(struct proc *p, struct proc_info *proc_info);

#endif
