#include "cpuckpt.h"
#include "fileio.h"
#include "_slsmm.h"
#include "debug.h"

#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/signalvar.h>
#include <machine/reg.h>

/*
 * Get the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
int
reg_dump(struct cpu_info **cpu_info, size_t *cpu_info_size, struct proc *p, 
        int fd)
{
    int error = 0;
    struct thread *td;

    *cpu_info_size = sizeof(struct cpu_info) * p->p_numthreads;
    *cpu_info = malloc(*cpu_info_size, M_SLSMM, M_NOWAIT);
    if (*cpu_info == NULL) {
        printf("ENOMEM\n");
        return ENOMEM;
    }

    /*
     * XXX We are assuming a single thread, so we use *cpu_info 
     * as a pointer to struct cpu_state 
     */
    FOREACH_THREAD_IN_PROC(p, td) {
        thread_lock(td);

        error = proc_read_regs(td, &(*cpu_info)->regs);
        if (error) {
	    thread_unlock(td);
            printf("CPU reg dump error %d\n", error);
            break;
        }


        error = proc_read_fpregs(td, &(*cpu_info)->fpregs);
        thread_unlock(td);
        if (error) {
            printf("CPU fpreg dump error %d\n", error);
            break;
        }

	    /* Assume single thread */
        break; 
    }

    return error;
}

/*
 * Set the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
int
reg_restore(struct proc *p, int fd)
{
    int error = 0;
    struct thread *td;
    struct cpu_info cpu_info;

    FOREACH_THREAD_IN_PROC(p, td) {
        error = fd_read(&cpu_info, sizeof(struct cpu_info), fd);
        if (error) break;

        error = proc_write_regs(td, &cpu_info.regs);
        error = proc_write_fpregs(td, &cpu_info.fpregs);
        if (error) break;


	    /* Assume single thread */
        break; 
    }
        

    return error;
}
