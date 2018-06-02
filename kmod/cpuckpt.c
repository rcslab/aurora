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

    PROC_LOCK(p);
    FOREACH_THREAD_IN_PROC(p, td) {
        proc_read_regs(td, &regs);
        fd_write(&regs, sizeof(struct reg), fd);

        break; //assume single thread now
    }
    PROC_UNLOCK(p);

    return error;
}

int
reg_restore(struct proc *p, int fd)
{
    int error = 0;
    struct thread *td;
    struct reg regs;

    PROC_LOCK(p);
    FOREACH_THREAD_IN_PROC(p, td) {
        fd_read(&regs, sizeof(struct reg), fd);
        proc_write_regs(td, &regs);

        break; //assume single thread now
    }
    PROC_UNLOCK(p);

    return error;
}
