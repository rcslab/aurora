#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

struct proc;
struct reg;

int reg_dump(struct proc *p, int fd);
int reg_restore(struct proc *p, int fd);

#endif
