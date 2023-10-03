#ifndef _METR_SYSCALL_H_
#define _METR_SYSCALL_H_

/* The Aurora syscall vector. */
extern struct sysentvec metrsys_sysvec;

void metropolis_initsysvec(void);
void metropolis_finisysvec(void);

#endif /* _METR_SYSCALL_H_ */
