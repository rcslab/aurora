#include <sys/types.h>

#include <sys/conf.h>
#include <sys/imgact.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sbuf.h>
#include <sys/signalvar.h>
#include <sys/sleepqueue.h>
#include <sys/tty.h>

#include <machine/cpufunc.h>
#include <machine/pcb.h>
#include <machine/reg.h>
#include <machine/sysarch.h>

#include "sls_kv.h"
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

/* This function in non-static but is not defined in any header. */
void exec_setregs(struct thread *td, struct image_params *imgp, u_long stack);

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
	struct image_params img;
	
	PROC_LOCK(td->td_proc);
	
	/* 
	 * We need to reset the threads' system registers as if
	 * we were calling execve(). For that, we need to call
	 * exec_setregs(), used during the syscall; the signature
	 * is a bit clumsy for our cases, since we do not have a
	 * process image, and only need one field from that struct,
	 * which we overwrite later anyway.
	 */
	img.entry_addr = td->td_frame->tf_rip;
	exec_setregs(td, &img, td->td_frame->tf_rsp);

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
	slsproc.pgid = p->p_pgid;
	slsproc.sid = p->p_session->s_sid;
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


/* XXX Find a better way to include system call vectors. */
extern struct sysentvec elf64_freebsd_sysvec;

/*
 * Set the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
slsrest_proc(struct proc *p, struct slsproc *slsproc, struct slsrest_data *restdata)
{
	struct sigacts *newsigacts, *oldsigacts;
	struct session *sess = NULL;
#if 0
	struct tty *oldtty, *newtty;
#endif
	struct pgrp *pgrp = NULL;
	int error;

	/* 
	 * XXX TEMP: Sessions are innately tied to terminals, so in order to be able
	 * to restore a whole session we need to be able to also restore a program
	 * such as tmux or mosh along with it. We aren't able to do that yet, so
	 * instead we have each pgrp be in the original restore proc's session for now.
	 */

	/* 
	 * Setting up process groups and sessions needs to be done carefully,
	 * because once a process is a group/session leader it cannot give
	 * up that role. Therefore we have to create process groups/sessions
	 * only for the leaders, and have the rest of the processes join them.
	 *
	 * This constraint unfortunately serializes the restore procedure, 
	 * since we need a barrier between pgrp/session creation and join.
	 */

	/* 
	 * We can't change the process group of a session leader. 
	 * This shouldn't be a problem, since we are a child of the
	 * original process which called us.
	 */
	KASSERT(!SESS_LEADER(p), "process is a session leader");
	/* Check if we were the original session leader. */
	if (slsproc->pid == slsproc->sid) {
	    pgrp = malloc(sizeof(*pgrp), M_SESSION, M_WAITOK);
	    sess = malloc(sizeof(*sess), M_PGRP, M_WAITOK);


	    /* We are changing the process tree with this. */
	    sx_xlock(&proctree_lock);

	    /* Create session. */
	    error = enterpgrp(p, p->p_pid, pgrp, sess);
	    sx_xunlock(&proctree_lock);
	    if (error != 0) {
		free(pgrp, M_SLSMM);
		free(sess, M_SLSMM);
		return (error);
	    }

	    MPASS(slsproc->sid == slsproc->pgid);

	    /* 
	     * Add the new pgrp and session to our tables. We take the lock to 
	     * serialize against non-leaders checking the hashtables. 
	     */
	    mtx_lock(&restdata->pgrpmtx);
	    error = slskv_add(restdata->sesstable, slsproc->sid, (uintptr_t) sess);
	    if (error != 0) {
		mtx_unlock(&restdata->pgrpmtx);
		goto error;
	    }

	    error = slskv_add(restdata->pgidtable, slsproc->pgid, (uintptr_t) pgrp);
	    if (error != 0) {
		mtx_unlock(&restdata->pgrpmtx);
		goto error;
	    }

	    /* Wake up any processes waiting to join the pgrp/session. */
	    cv_signal(&restdata->pgrpcv);
	    mtx_unlock(&restdata->pgrpmtx);

	} else if (slsproc->pid == slsproc->pgid) {
	    /* Otherwise we just create a new pgrp if we were the group leader. */
	    pgrp = malloc(sizeof(*pgrp), M_SLSMM, M_WAITOK);

	    /* Create just the pgrp. */
	    sx_xlock(&proctree_lock);
	    /* We are changing the process tree with this. */
	    error = enterpgrp(p, p->p_pid, pgrp, NULL);
	    sx_xunlock(&proctree_lock);
	    if (error != 0) {
		free(pgrp, M_SLSMM);
		return (error);
	    }

	    mtx_lock(&restdata->pgrpmtx);
	    error = slskv_add(restdata->pgidtable, slsproc->pgid, (uintptr_t) pgrp);
	    if (error != 0) {
		mtx_unlock(&restdata->pgrpmtx);
		goto error;
	    }


	    /* Wake up any processes waiting to join the pgrp. */
	    mtx_unlock(&restdata->pgrpmtx);
	}

	/* Do the thing, but have it be mutually exclusive w/ pgrp creation.  */
	mtx_lock(&restdata->pgrpmtx);
	for (;;) {
	    /* Check if the pgrp we need is available. */
	    if ((slsproc->pid != slsproc->pgid) && 
		(slskv_find(restdata->pgidtable, slsproc->pgid, (uintptr_t *) &pgrp) != 0)) {
		cv_wait(&restdata->pgrpcv, &restdata->pgrpmtx);
		continue;
	    }

	    /* XXX Commented out because the session we need is _always_ there right now. */
#if 0
	    /* Check if the session we need is available. */
	    if ((slsproc->pid != slsproc->sid) && 
		(slskv_find(restdata->sesstable, slsproc->sid, (uintptr_t *) &sess) != 0)) {
		cv_wait(&restdata->pgrpcv, &restdata->pgrpmtx);
		continue;
	    }
#endif

	    break;
	}
	mtx_unlock(&restdata->pgrpmtx);

	/* If we need to enter a pgroup, look it up in the tables and do it here. */
	if (p->p_pid != p->p_pgid) {
	    error = slskv_find(restdata->pgidtable, slsproc->pgid, (uint64_t *) &pgrp);
	    KASSERT(error == 0, "restored pgrp not found");

	    sx_xlock(&proctree_lock);
	    error = enterthispgrp(p, pgrp);
	    sx_xunlock(&proctree_lock);
	    if (error != 0) {
		/* Turn it back into null so we don't free it while error handling. */
		pgrp = NULL;
		goto error;
	    }

	} else if (!SESS_LEADER(p)) {
	    /* XXX Keep it in the same session. */
#if 0
	    /* We are the pgrp leader, but not the session leader. */
	    error = slskv_find(restdata->sesstable, slsproc->sid, (uintptr_t *) &sess);
	    KASSERT(error == 0, "restored session not found");

	    /* Associate the pgrp with the session. */
	    sx_xlock(&proctree_lock);

	    
	    /* If we are in the foreground of our tty, make sure we leave it. */
	    oldtty = pgrp->pg_session->s_ttyp;
	    newtty = sess->s_ttyp;
	    if (oldtty != newtty) {
		tty_lock(oldtty);
		tty_rel_pgrp(oldtty, pgrp);
	    }

	    /* 
	     * Associate with the new session and dissociate with the old one. 
	     * These operations need to happen in that order, because if the old
	     * and the new sessions coincide, and the old session is only referred
	     * to by us, dropping our reference causes it to be destroyed.
	     */
	    sess_hold(pgrp->pg_session);
	    sess_release(pgrp->pg_session);
	    pgrp->pg_session = sess;

	    sx_xunlock(&proctree_lock);
#endif
	}

	/* We bcopy the exact way it's done in sigacts_copy(). */
	newsigacts = sigacts_alloc();
	bcopy(&slsproc->sigacts, newsigacts, offsetof(struct sigacts, ps_refcnt));

	oldsigacts = p->p_sigacts;
	p->p_sigacts = newsigacts;
	/* Restore the standard syscall vector.*/
	/* XXX Allow for arbitrary syscall vectors. */
	p->p_sysent = &elf64_freebsd_sysvec;
	sigacts_free(oldsigacts);

	/* Restore the standard syscall vector.*/

	/* 
	 * XXX Allow for arbitrary syscall vectors.
	 * The correct way would be to find out what kind of vector
	 * this is at checkpoint time, encoding it as a value in the
	 * process, then doing the opposite at restore time (finding
	 * the correct vector by using the value as an index in a table).
	 */


	return (0);

error:


	return (error);
}

