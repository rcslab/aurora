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

#include "sls_mm.h"
#include "sls_proc.h"

/*
 * Get the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
static int
slsckpt_thread(struct thread *td, struct sbuf *sb)
{
	int error = 0;
	struct slsthread slsthread;

	error = proc_read_regs(td, &slsthread.regs);
	if (error != 0)
	    return error;

	error = proc_read_fpregs(td, &slsthread.fpregs);
	if (error != 0)
	    return error;

	bcopy(&td->td_sigmask, &slsthread.sigmask, sizeof(sigset_t));
	bcopy(&td->td_oldsigmask, &slsthread.oldsigmask, sizeof(sigset_t));

	slsthread.tid = td->td_tid;
	slsthread.fs_base = td->td_pcb->pcb_fsbase;
	slsthread.magic = SLSTHREAD_ID;
	slsthread.slsid = (uint64_t) td;

	error = sbuf_bcat(sb, (void *) &slsthread, sizeof(slsthread));
	if (error != 0)
	    return error;

	return 0;
}

/*
 * Set the state of all threads of the process. This function
 * takes and leaves the process locked. The slsthread struct pointer
 * is passed as a thunk to satisfy the signature of thread_create, to
 * which thread_rest is an argument.
 */
static int
sls_thread_create(struct thread *td, void *thunk)
{
	int error = 0;
	struct slsthread *slsthread = (struct slsthread *) thunk;

	PROC_LOCK(td->td_proc);
	error = proc_write_regs(td, &slsthread->regs);
	if (error != 0)
	    goto done;
	
	error = proc_write_fpregs(td, &slsthread->fpregs);
	if (error != 0)
	    goto done;

	bcopy(&slsthread->sigmask, &td->td_sigmask, sizeof(sigset_t));
	bcopy(&slsthread->oldsigmask, &td->td_oldsigmask, sizeof(sigset_t));

	set_pcb_flags(td->td_pcb, PCB_FULL_IRET);
	td->td_pcb->pcb_fsbase = slsthread->fs_base;

done:

	PROC_UNLOCK(td->td_proc);
	return error;
}

int
slsrest_thread(struct proc *p, struct slsthread *slsthread)
{
	int error;
	
	PROC_UNLOCK(p);
	error = thread_create(curthread, NULL, sls_thread_create, 
	    (void *) slsthread);
	PROC_LOCK(p);

	return error;
}

/*
 * Get the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
slsckpt_proc(struct proc *p, struct sbuf *sb)
{
	struct thread *td;
	int error = 0;
	struct sigacts *sigacts;
	struct slsproc slsproc;

	slsproc.nthreads = p->p_numthreads;
	slsproc.pid = p->p_pid;
	slsproc.magic = SLSPROC_ID;
	slsproc.slsid = (uint64_t) p;

	sigacts = p->p_sigacts;

	mtx_lock(&sigacts->ps_mtx);
	bcopy(sigacts, &slsproc.sigacts, offsetof(struct sigacts, ps_refcnt));
	mtx_unlock(&sigacts->ps_mtx);

	error = sbuf_bcat(sb, (void *) &slsproc, sizeof(slsproc));
	if (error != 0)
	    return ENOMEM;

	FOREACH_THREAD_IN_PROC(p, td) {
	    thread_lock(td);
	    error = slsckpt_thread(td, sb);
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
slsrest_proc(struct proc *p, struct slsproc *slsproc)
{
	struct sigacts *newsigacts, *oldsigacts;

	/* We bcopy the exact way it's done in sigacts_copy(). */
	newsigacts = sigacts_alloc();
	bcopy(&slsproc->sigacts, newsigacts, offsetof(struct sigacts, ps_refcnt));

	oldsigacts = p->p_sigacts;
	p->p_sigacts = newsigacts;
	sigacts_free(oldsigacts);

	return 0;
}

