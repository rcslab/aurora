#ifndef _SLS_SYSCALL_H_
#define _SLS_SYSCALL_H_

void slssyscall_initsysvec(void);
void slssyscall_finisysvec(void);

void sls_exit_procremove(struct proc *p);
extern void (*sls_exit_hook)(struct proc *p);

extern struct sysentvec slssyscall_sysvec;

#endif /* _SLS_SYSCALL_H_ */
