#ifndef _CPUCKPT_H_
#define _CPUCKPT_H_

#include <sys/types.h>

struct proc;
struct reg;

int reg_dump(void **buffer, size_t *buf_size, struct proc *p, int fd);
int reg_restore(struct proc *p, int fd);

#endif
