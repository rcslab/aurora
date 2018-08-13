#include "cpuckpt.h"
#include "fileio.h"
#include "_slsmm.h"

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
thread_checkpoint(struct proc *p, struct thread_info *thread_info)
{
	struct thread *td;
	int threadno;
	int error = 0;

	threadno = 0;
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);

		error = proc_read_regs(td, &thread_info[threadno].regs);
		if (error) {
			thread_unlock(td);
			printf("CPU reg dump error %d\n", error);
			break;
		}

		error = proc_read_fpregs(td, &thread_info[threadno].fpregs);
		thread_unlock(td);
		if (error) {
			printf("CPU fpreg dump error %d\n", error);
			break;
		}

	}

	return error;
}

/*
 * Set the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
int
thread_restore(struct proc *p, struct thread_info *thread_info)
{
	int error = 0;
	struct thread *td;
	int threadno;

	/* 
	 * XXX: We assume that the number of threads of the process to be restored
	 * is the same one as that of the process to be overwritten. This is extremely
	 * not true most of the time.
	 */

	threadno = 0;
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);

		error = proc_write_regs(td, &thread_info[threadno].regs);
		error = proc_write_fpregs(td, &thread_info[threadno].fpregs);
		thread_info[threadno].tid = td->td_tid;

		thread_unlock(td);
		if (error) break;
	}

	return error;
}

/*
 * Get the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
proc_checkpoint(struct proc *p, struct proc_info *proc_info)
{
	proc_info->nthreads = p->p_numthreads;
	proc_info->pid = p->p_pid;

	/*
	 * TODO: Checkpoint file descriptors. Maybe check if a
	 * file descriptor is a regular file before checkpointing? 
	 * If it's a special file that corresponds to stuff we cannot
	 * checkpoint, it is no use storing it.
	 */

	return 0;
}

/*
 * Set the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
proc_restore(struct proc *p, struct proc_info *proc_info)
{
	/* TODO: Change PID if possible (or even feasible) */
	/*
	 * TODO: Change the number of threads in the process 
	 * to match those of the checkpointed process 
	 */
	/*
	 * TODO: Restore file descriptors. See proc_checkpoint for thoughts
	 * on that.
	 */

	return 0;
}

