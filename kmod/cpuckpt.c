#include <sys/types.h>

#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/signalvar.h>
#include <sys/sleepqueue.h>

#include <machine/cpufunc.h>
#include <machine/pcb.h>
#include <machine/reg.h>
#include <machine/sysarch.h>

#include "cpuckpt.h"
#include "fileio.h"
#include "_slsmm.h"

/*
 * Get the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
static int
thread_checkpoint(struct thread *td, struct thread_info *thread_info)
{
	int error = 0;

	/* We are doing the work of thr_new_initthr */
	cpu_thread_clean(td);

	error = proc_read_regs(td, &thread_info->regs);
	if (error) {
		thread_unlock(td);
		printf("CPU reg dump error %d\n", error);
		return 0;
	}

	error = proc_read_fpregs(td, &thread_info->fpregs);
	if (error) {
		printf("CPU fpreg dump error %d\n", error);
		return 0;
	}

	bcopy(&td->td_sigmask, &thread_info->sigmask, sizeof(sigset_t));
	bcopy(&td->td_oldsigmask, &thread_info->oldsigmask, sizeof(sigset_t));

	thread_info->tid = td->td_tid;
	thread_info->fs_base = td->td_pcb->pcb_fsbase;

	return error;
}

/*
 * Set the state of all threads of the process. This function
 * takes and leaves the process locked. The thread_info struct pointer
 * is passed as a thunk to satisfy the signature of thread_create, to
 * which thread_restore is an argument.
 */
static int
thread_restore(struct thread *td, void *thunk)
{
	int error = 0;
	struct thread_info *thread_info = (struct thread_info *) thunk;

	error = proc_write_regs(td, &thread_info->regs);
	error = proc_write_fpregs(td, &thread_info->fpregs);

	bcopy(&thread_info->sigmask, &td->td_sigmask, sizeof(sigset_t));
	bcopy(&thread_info->oldsigmask, &td->td_oldsigmask, sizeof(sigset_t));

	/*
	 * Yeah, not a good idea (for now).
	thread_info->tid = td->td_tid;
	*/

	set_pcb_flags(td->td_pcb, PCB_FULL_IRET);
	td->td_pcb->pcb_fsbase = thread_info->fs_base;

	return error;
}

/*
 * Get the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
proc_checkpoint(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos)
{
	struct thread *td;
	int threadno;

	proc_info->nthreads = p->p_numthreads;
	proc_info->pid = p->p_pid;

	/*
	 * TODO: Checkpoint file descriptors. Maybe check if a
	 * file descriptor is a regular file before checkpointing? 
	 * If it's a special file that corresponds to stuff we cannot
	 * checkpoint, it is no use storing it.
	 */

	sigacts_copy(&proc_info->sigacts, p->p_sigacts);

	threadno = 0;
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);
		thread_checkpoint(td, &thread_infos[threadno]);
		thread_unlock(td);
		threadno++;
	}

	return 0;
}


/*
 * No need to initialize the thread in proc_restore(), we are going to do that
 * in thread_restore() with actual data. This function is used as a no-op
 * in thread_create() below.
 */
static int 
initialize_thread_noop(struct thread *td, void *thunk)
{
	return 0;
}

/*
 * Set the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
proc_restore(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos)
{
	struct sigacts *newsigacts, *oldsigacts;
	struct thread *td __unused;
	int threadno;

	/* TODO: Change PID if possible (or even feasible) */
	/*
	 * TODO: Change the number of threads in the process 
	 * to match those of the checkpointed process 
	 */
	/*
	 * TODO: Restore file descriptors. See proc_checkpoint for thoughts
	 * on that.
	 */

	/*
	 * We bcopy the exact way it's done in sigacts_copy().
	 */
	newsigacts = sigacts_alloc();
	bcopy(&proc_info->sigacts, newsigacts, offsetof(struct sigacts, ps_refcnt));

	oldsigacts = p->p_sigacts;
	p->p_sigacts = newsigacts;
	sigacts_free(oldsigacts);
	
	/* The first thread of the new process is this one. */
	thread_lock(curthread);
	thread_restore(curthread, (void *) &thread_infos[0]);
	thread_unlock(curthread);

	for (threadno = 1; threadno < proc_info->nthreads; threadno++) {
		thread_create(curthread, NULL, thread_restore, (void *) &thread_infos[threadno]);
		//thread_create(curthread, NULL, initialize_thread_noop, NULL);
	}

	return 0;
}

