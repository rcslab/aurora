#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

#include <sys/types.h>

#include "slsmm.h"

struct proc;
struct reg;
struct fpreg;

int proc_checkpoint(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos);
int proc_restore(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos);

#endif
