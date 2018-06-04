#include "cpuckpt.h"
#include "fileio.h"

#include <sys/conf.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>

#include <machine/reg.h>

int
reg_dump(struct proc *p, int fd)
{
    int error = 0;
    struct thread *td;
    struct reg regs;
    struct fpreg fpregs;

    PROC_LOCK(p);
    _PHOLD(p);
    FOREACH_THREAD_IN_PROC(p, td) {
        PROC_SLOCK(p);
        thread_lock(td);
        thread_suspend_one(td);
        thread_unlock(td);

        error = proc_read_regs(td, &regs);
        error = proc_read_fpregs(td, &fpregs);
        if (error) break;
        thread_unsuspend(p);
        PROC_SUNLOCK(p);

        error = fd_write(&regs, sizeof(struct reg), fd);
        error = fd_write(&fpregs, sizeof(struct fpreg), fd);
        if (error) break;

        break; //assume single thread now
    }
    _PRELE(p);
    PROC_UNLOCK(p);

    return error;
}

int
reg_restore(struct proc *p, int fd)
{
    int error = 0;
    struct thread *td;
    struct reg regs;
    struct fpreg fpregs;

    PROC_LOCK(p);
    _PHOLD(p);
    FOREACH_THREAD_IN_PROC(p, td) {
        error = fd_read(&regs, sizeof(struct reg), fd);
        error = fd_read(&fpregs, sizeof(struct fpreg), fd);
        if (error) break;

        PROC_SLOCK(p);
        /*
        thread_lock(td);
        thread_suspend_one(td);
        thread_unlock(td);
        */

        error = proc_write_regs(td, &regs);
        error = proc_write_fpregs(td, &fpregs);
        if (error) break;

        //thread_unsuspend(p);
        PROC_SUNLOCK(p);

        break; //assume single thread now
    }
    _PRELE(p);
    PROC_UNLOCK(p);

    return error;
}
