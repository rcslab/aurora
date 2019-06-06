#include <sys/types.h>

#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sbuf.h>
#include <sys/signalvar.h>
#include <sys/sleepqueue.h>

#include <machine/cpufunc.h>
#include <machine/pcb.h>
#include <machine/reg.h>
#include <machine/sysarch.h>

#include "slsmm.h"
#include "sls_proc.h"

/*
 * Get the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
static int
thread_ckpt(struct thread *td, struct sbuf *sb)
{
	int error = 0;
	struct thread_info thread_info;

	error = proc_read_regs(td, &thread_info.regs);
	if (error != 0)
	    return error;

	error = proc_read_fpregs(td, &thread_info.fpregs);
	if (error != 0)
	    return error;

	bcopy(&td->td_sigmask, &thread_info.sigmask, sizeof(sigset_t));
	bcopy(&td->td_oldsigmask, &thread_info.oldsigmask, sizeof(sigset_t));

	thread_info.tid = td->td_tid;
	thread_info.fs_base = td->td_pcb->pcb_fsbase;
	thread_info.magic = SLS_THREAD_INFO_MAGIC;

	error = sbuf_bcat(sb, (void *) &thread_info, sizeof(thread_info));
	if (error != 0)
	    return error;

	return 0;
}

/*
 * Set the state of all threads of the process. This function
 * takes and leaves the process locked. The thread_info struct pointer
 * is passed as a thunk to satisfy the signature of thread_create, to
 * which thread_rest is an argument.
 */
static int
thread_rest(struct thread *td, void *thunk)
{
	int error = 0;
	struct thread_info *thread_info = (struct thread_info *) thunk;

	PROC_LOCK(td->td_proc);
	error = proc_write_regs(td, &thread_info->regs);
	if (error != 0)
	    goto rest_done;
	
	error = proc_write_fpregs(td, &thread_info->fpregs);
	if (error != 0)
	    goto rest_done;

	bcopy(&thread_info->sigmask, &td->td_sigmask, sizeof(sigset_t));
	bcopy(&thread_info->oldsigmask, &td->td_oldsigmask, sizeof(sigset_t));

	set_pcb_flags(td->td_pcb, PCB_FULL_IRET);
	td->td_pcb->pcb_fsbase = thread_info->fs_base;

rest_done:

	PROC_UNLOCK(td->td_proc);
	return error;
}

/*
 * Get the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
proc_ckpt(struct proc *p, struct sbuf *sb)
{
	struct thread *td;
	int error = 0;
	struct sigacts *sigacts;
	struct proc_info proc_info;

	proc_info.nthreads = p->p_numthreads;
	proc_info.pid = p->p_pid;
	proc_info.magic = SLS_PROC_INFO_MAGIC;

	sigacts = p->p_sigacts;

	mtx_lock(&sigacts->ps_mtx);
	bcopy(sigacts, &proc_info.sigacts, offsetof(struct sigacts, ps_refcnt));
	mtx_unlock(&sigacts->ps_mtx);

	error = sbuf_bcat(sb, (void *) &proc_info, sizeof(proc_info));
	if (error != 0)
	    return ENOMEM;

	FOREACH_THREAD_IN_PROC(p, td) {
	    thread_lock(td);
	    error = thread_ckpt(td, sb);
	    thread_unlock(td);
	    if (error != 0)
		return error;
	}

	return 0;
}


/*
 * Set the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
proc_rest(struct proc *p, struct proc_info *proc_info, struct thread_info *thread_infos)
{
	struct sigacts *newsigacts, *oldsigacts;
	struct thread *td __unused;
	int error = 0;
	int threadno;

	/* We bcopy the exact way it's done in sigacts_copy(). */
	newsigacts = sigacts_alloc();
	bcopy(&proc_info->sigacts, newsigacts, offsetof(struct sigacts, ps_refcnt));

	oldsigacts = p->p_sigacts;
	p->p_sigacts = newsigacts;
	sigacts_free(oldsigacts);

	PROC_UNLOCK(p);
	for (threadno = 0; threadno < proc_info->nthreads; threadno++) {
	    error = thread_create(curthread, NULL, thread_rest, 
		(void *) &thread_infos[threadno]);
	    if (error != 0) {
		PROC_LOCK(p);
		return error;
	    }
	}
	PROC_LOCK(p);

	return 0;
}

