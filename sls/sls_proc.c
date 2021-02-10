#include <sys/types.h>
#include <sys/selinfo.h>
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

#include "debug.h"
#include "sls_kv.h"
#include "sls_proc.h"
#include "sls_vnode.h"

/*
 * Get the state of all threads of the process. This function
 * takes and leaves the process locked.
 */
static int
slsckpt_thread(struct thread *td, struct sbuf *sb)
{
	int error = 0;
	struct slsthread slsthread;

	PROC_LOCK(td->td_proc);
	thread_lock(td);

	error = proc_read_regs(td, &slsthread.regs);
	if (error != 0)
		return error;

	error = proc_read_fpregs(td, &slsthread.fpregs);
	if (error != 0)
		return error;

	bcopy(&td->td_sigmask, &slsthread.sigmask, sizeof(sigset_t));
	bcopy(&td->td_oldsigmask, &slsthread.oldsigmask, sizeof(sigset_t));

	DEBUG1("Saving thread %x", td->td_tid);
	slsthread.tid = td->td_tid;
	slsthread.fs_base = td->td_pcb->pcb_fsbase;
	slsthread.magic = SLSTHREAD_ID;
	slsthread.slsid = (uint64_t)td;
	slsthread.tf_err = td->td_frame->tf_err;
	slsthread.tf_trapno = td->td_frame->tf_trapno;

	thread_unlock(td);
	PROC_UNLOCK(td->td_proc);

	error = sbuf_bcat(sb, (void *)&slsthread, sizeof(slsthread));
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
	struct slsthread *slsthread = (struct slsthread *)thunk;
	struct image_params img;

	DEBUG1("Created thread %p", td);

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

	td->td_frame->tf_err = slsthread->tf_err;
	td->td_frame->tf_trapno = slsthread->tf_trapno;

	DEBUG1("Restoring thread %d", td->td_tid);
	DEBUG4("rax 0x%lx rbx 0x%lx rcx 0x%lx rdx 0x%lx", td->td_frame->tf_rax,
	    td->td_frame->tf_rbx, td->td_frame->tf_rcx, td->td_frame->tf_rdx);
	DEBUG4("rdi 0x%lx rsi 0x%lx rip 0x%lx rflags 0x%lx",
	    td->td_frame->tf_rdi, td->td_frame->tf_rsi, td->td_frame->tf_rip,
	    td->td_frame->tf_rflags);
	DEBUG4("r8 0x%lx r9 0x%lx r10 0x%lx r11 0x%lx", td->td_frame->tf_r8,
	    td->td_frame->tf_r9, td->td_frame->tf_r10, td->td_frame->tf_r11);
	DEBUG4("r12 0x%lx r13 0x%lx r14 0x%lx r15 0x%lx", td->td_frame->tf_r12,
	    td->td_frame->tf_r13, td->td_frame->tf_r14, td->td_frame->tf_r15);
	DEBUG4("cs 0x%x ds 0x%x es 0x%x ss 0x%x", td->td_frame->tf_cs,
	    td->td_frame->tf_ds, td->td_frame->tf_es, td->td_frame->tf_ss);
	DEBUG5("rsp 0x%lx err 0x%lx addr 0x%lx trapno 0x%x flags 0x%x",
	    td->td_frame->tf_rsp, td->td_frame->tf_err, td->td_frame->tf_addr,
	    td->td_frame->tf_trapno, td->td_frame->tf_flags);

done:

	/* Have the thread check itself for suspension at the kernel boundary.
	 */
	td->td_flags |= TDF_NEEDSUSPCHK;
	PROC_UNLOCK(td->td_proc);

	return (error);
}

int
slsrest_thread(struct proc *p, struct slsthread *slsthread)
{
	int error;

	DEBUG1("Trying to restore thread with tid %lu", slsthread->tid);
	/* XXX Pass the thread ID we wish the restored thread to have. */
	error = thread_create(
	    curthread, NULL, sls_thread_create, (void *)slsthread);

	return (error);
}

/*
 * Get the process state, including file descriptors, sockets, and metadata
 * like PIDs. This function takes and leaves the process locked.
 */
int
slsckpt_proc(struct proc *p, struct sbuf *sb, slsset *procset,
    struct slsckpt_data *sckpt_data)
{
	struct thread *td;
	int error = 0;
	struct sigacts *sigacts;
	struct slsproc slsproc;
	struct proc *pleader;

	PROC_LOCK(p);
	slsproc.nthreads = p->p_numthreads;
	slsproc.pid = p->p_pid;
	/*
	 * If the parent of the process is in the SLS, we use the pptr
	 * field to store its slsid. Otherwise we denote that the process
	 * is an orphan by using its own slsid as that of its parent.
	 */
	if (slsset_find_unlocked(procset, (uint64_t)p->p_pptr) == 0)
		slsproc.pptr = (uint64_t)p->p_pptr;
	else
		slsproc.pptr = (uint64_t)p;

	/*
	 * Similarly, if the session leader is not in the SLS, the  process
	 * process is going to be migrated to the restored process' session.
	 */
	if (slsset_find_unlocked(procset, (uint64_t)p->p_session->s_leader) ==
	    0)
		slsproc.sid = (uint64_t)p->p_session->s_sid;
	else
		slsproc.sid = (uint64_t)0;

	/*
	 * Process groups can be leaderless. We need to know whether
	 * group's leader is in the SLS, because if it is we need to
	 * let the leader create it; otherwise, we let a random process
	 * create it. (XXX: This can be a problem if the process wants
	 * to create a new pgrp afterwards; it can't, because it is
	 * already the leader of one. The way to solve this is either to
	 * a) create a random sibling of the process that just creates
	 * a new pgroup, waits for others to enter it, then exits, or to
	 * b) manually create a pgroup that has a pid that does not
	 * correspond to any process. This, however, is dangerous if
	 * a process with a pid equal to that of the pgroup is created
	 * at a later time.
	 */
	slsproc.pgid = p->p_pgid;
	/* Try to find the process leader. */

	PROC_UNLOCK(p);
	error = pget(p->p_pgid, PGET_WANTREAD, &pleader);
	PROC_LOCK(p);

	if (error != 0) {
		/* It doesn't even exist, so the group is leaderless. */
		slsproc.pgrpwait = 0;
	} else {
		/* Check if it's in the process set. */
		if (slsset_find_unlocked(procset, (uint64_t)pleader) == 0)
			slsproc.pgrpwait = 1;
		else
			slsproc.pgrpwait = 0;

		/* If we're the leader, we're already locked. */
		if (pleader == p)
			_PRELE(p);
		else
			PRELE(pleader);
	}

	slsproc.magic = SLSPROC_ID;
	slsproc.slsid = (uint64_t)p;
	/* Get the name of the process. */
	memcpy(slsproc.name, p->p_comm, MAXCOMLEN + 1);

	sigacts = p->p_sigacts;

	mtx_lock(&sigacts->ps_mtx);
	bcopy(sigacts, &slsproc.sigacts, offsetof(struct sigacts, ps_refcnt));
	mtx_unlock(&sigacts->ps_mtx);

	PROC_UNLOCK(p);

	/* Save the path of the executable. */
	KASSERT(p->p_textvp != NULL, ("process %p has no text vnode", p));
	slsproc.textvp = (uint64_t)p->p_textvp;
	error = slsckpt_vnode(p->p_textvp, sckpt_data);
	if (error != 0)
		return (error);

	error = sbuf_bcat(sb, (void *)&slsproc, sizeof(slsproc));
	if (error != 0)
		return (error);

	/* Checkpoint each thread individually. */
	FOREACH_THREAD_IN_PROC (p, td) {
		error = slsckpt_thread(td, sb);
		if (error != 0)
			return (error);
	}

	return (0);
}

/* XXX Find a better way to include system call vectors. */
extern struct sysentvec elf64_freebsd_sysvec;
/*
 * XXX Create our own custom syscall vector for future use.
 * It can be used, for example, for interposing between
 * old and new PIDs/TIDs.
 */

/*
 * Create a new process group and session, properly locking while doing it.
 */
static int
slsrest_enterpgrp(struct proc *p, struct pgrp *pgrp, struct session *sess)
{
	int error;

	PROC_LOCK_ASSERT(p, MA_OWNED);
	/* We are changing the process tree with this. */
	PROC_UNLOCK(p);
	sx_xlock(&proctree_lock);

	/* Create session. */
	error = enterpgrp(p, p->p_pid, pgrp, sess);
	sx_xunlock(&proctree_lock);

	PROC_LOCK(p);

	return (error);
}

static int
slsrest_proc_reparent(
    struct slsproc *slsproc, struct slsrest_data *restdata, int daemon)
{
	struct proc *p = curproc;
	struct proc *pptr;
	int error;

	sx_xlock(&proctree_lock);
	if (slsproc->pptr != slsproc->slsid) {
		DEBUG1("Reparenting process %d", slsproc->pid);
		/* If the parent is in the SLS, it must have been restored. */
		error = slskv_find(
		    restdata->proctable, slsproc->pptr, (uintptr_t *)&pptr);
		KASSERT(error == 0, ("restored pptr not found"));

		proc_reparent(p, pptr, 1);
	} else if (daemon != 0) {
		proc_reparent(p, p->p_reaper, true);
	}
	sx_xunlock(&proctree_lock);
	DEBUG2("Parent for %d is %d", slsproc->pid, p->p_pptr->p_pid);

	return (0);
}

static int
slsrest_proc_signals(struct slsproc *slsproc, struct slsrest_data *restdata)
{
	struct sigacts *newsigacts, *oldsigacts;
	struct proc *p = curproc;

	/* We bcopy the exact way it's done in sigacts_copy(). */
	newsigacts = sigacts_alloc();
	bcopy(
	    &slsproc->sigacts, newsigacts, offsetof(struct sigacts, ps_refcnt));

	oldsigacts = p->p_sigacts;
	p->p_sigacts = newsigacts;

	sigacts_free(oldsigacts);

	return (0);
}

static int
slsrest_proc_textvp(struct slsproc *slsproc, struct slsrest_data *restdata)
{
	struct proc *p = curproc;
	struct vnode *textvp;
	int error;

	/* Restore the original executable pointer. */
	error = slskv_find(
	    restdata->vntable, slsproc->textvp, (uintptr_t *)&textvp);
	if (error != 0)
		return (error);

	/* Restore the executable name and path. */
	memcpy(p->p_comm, slsproc->name, MAXCOMLEN + 1);
	vref(textvp);

	vrele(p->p_textvp);
	p->p_textvp = textvp;

	return (0);
}

static int
slsrest_session(struct slsproc *slsproc, struct slsrest_data *restdata)
{
	struct proc *p = curproc;
	struct pgrp *pgrp = NULL;
	struct session *sess;
	int error;

	DEBUG1("Restoring session leader %ld", slsproc->pid);

	pgrp = malloc(sizeof(*pgrp), M_PGRP, M_WAITOK | M_ZERO);
	if (pgrp == NULL)
		return (ENOMEM);

	sess = malloc(sizeof(*sess), M_SESSION, M_WAITOK | M_ZERO);
	if (sess == NULL) {
		free(pgrp, M_PGRP);
		return (ENOMEM);
	}

	/* Create the new session. */
	error = slsrest_enterpgrp(p, pgrp, sess);
	if (error != 0) {
		free(pgrp, M_PGRP);
		free(sess, M_SESSION);
		return (error);
	}

	/*
	 * After this point the group and session are in the system, we cannot
	 * clean them up in case of an error.
	 */

	/* Session leaders have to be group leaders. */
	MPASS(slsproc->sid == slsproc->pgid);

	/*
	 * Add the new pgrp and session to our tables. We take the lock to
	 * serialize against non-leaders checking the hashtables.
	 */
	mtx_lock(&restdata->procmtx);
	sess_hold(sess);
	error = slskv_add(restdata->sesstable, slsproc->sid, (uintptr_t)sess);
	if (error != 0) {
		sess_release(sess);
		mtx_unlock(&restdata->procmtx);
		return (error);
	}

	error = slskv_add(restdata->pgidtable, slsproc->pgid, (uintptr_t)pgrp);
	if (error != 0) {
		mtx_unlock(&restdata->procmtx);
		return (error);
	}

	/* Wake up any processes waiting to join the pgrp/session. */
	mtx_unlock(&restdata->procmtx);

	return (0);
}

static int
slsrest_pgrp(struct slsproc *slsproc, struct slsrest_data *restdata, int daemon)
{
	struct proc *p = curproc;
	struct pgrp *pgrp;
	int error;

	DEBUG1("Restoring group leader %ld", slsproc->pid);
	if (daemon != 0) {
		DEBUG("Daemon mode, creating new process group");

		/* Otherwise we just create a new pgrp if we were the group
		 * leader. */
		pgrp = malloc(sizeof(*pgrp), M_PGRP, M_WAITOK | M_ZERO);
		if (pgrp == NULL)
			return (ENOMEM);

		/* Create just the pgrp. */
		error = slsrest_enterpgrp(p, pgrp, NULL);
		if (error != 0) {
			free(pgrp, M_PGRP);
			return (error);
		}

	} else {
		DEBUG("Not in daemon mode, staying in the caller's pgroup");

		/*
		 * We want nondaemon processes in the SLS to be able to receive
		 * SIGTERM signals for sls_restore(). The code above should be
		 * used for daemons, but for the rest it essentially detaches
		 * the process from the terminal.
		 */
		slsproc->pgrpwait = 0;
		pgrp = p->p_pgrp;
	}

	/* Wake up any processes waiting to join the pgrp. */
	mtx_lock(&restdata->procmtx);
	error = slskv_add(restdata->pgidtable, slsproc->pgid, (uintptr_t)pgrp);
	mtx_unlock(&restdata->procmtx);
	if (error != 0)
		return (error);

	return (0);
}

/*
 * Wait until the sessions and groups of the process are created.
 */
static void
slsrest_procwait(struct slsproc *slsproc, struct slsrest_data *restdata)
{
	struct sesssion *sess;
	struct pgrp *pgrp;
	struct proc *pptr;

	mtx_lock(&restdata->procmtx);
	for (;;) {
		DEBUG("Waiting on all process groups and sessions");
		/* Check if the pgrp we need is available. */
		if ((slsproc->pid != slsproc->pgid) &&
		    (slsproc->pgrpwait != 0) &&
		    (slskv_find(restdata->pgidtable, slsproc->pgid,
			 (uintptr_t *)&pgrp) != 0)) {
			cv_wait(&restdata->proccv, &restdata->procmtx);
			continue;
		}

		/* Check if the session we need is available. */
		if ((slsproc->pid != slsproc->sid) && (slsproc->sid != 0) &&
		    (slskv_find(restdata->sesstable, slsproc->sid,
			 (uintptr_t *)&sess) != 0)) {
			cv_wait(&restdata->proccv, &restdata->procmtx);
			continue;
		}

		/* Check if our parent has been restored, if it exists. */
		if ((slsproc->slsid != slsproc->pptr) &&
		    (slskv_find(restdata->proctable, slsproc->pptr,
			 (uintptr_t *)&pptr) != 0)) {
			cv_wait(&restdata->proccv, &restdata->procmtx);
			continue;
		}

		break;
	}
	mtx_unlock(&restdata->procmtx);
}

/*
 * Attach the process to a newly restored process group.
 */
static int
slsrest_attach_pgrp(int pgid, struct slsrest_data *restdata)
{
	struct pgrp *pgrp;
	int error;

	error = slskv_find(restdata->pgidtable, pgid, (uint64_t *)&pgrp);
	KASSERT(error == 0, ("restored pgrp not found"));

	/*
	 * We might already be in the right pgrp, if that is the group
	 * of the parent process that called sls_restore().
	 */

	/*
	 * XXX Actually implement Enter the session of the pgroup, too. This
	 * whole block was complex and buggy, so it has been removed for now.
	 */

	return (0);
}

/*
 * Attach a group leader to a to a newly restored session.
 */
static int
slsrest_attach_sess(int sid, struct slsrest_data *restdata)
{
	struct proc *p = curproc;
	struct pgrp *pgrp = p->p_pgrp;
	struct tty *oldtty, *newtty;
	struct session *sess;
	int error;

	/* We are the pgrp leader, but not the session leader. */
	error = slskv_find(restdata->sesstable, sid, (uintptr_t *)&sess);
	KASSERT(error == 0, ("restored session not found"));

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
	sess_hold(sess);
	sess_release(pgrp->pg_session);
	pgrp->pg_session = sess;

	sx_xunlock(&proctree_lock);

	return (0);
}

/*
 * Set the process state, including process tree relations and
 * like PIDs. This function takes and leaves the process locked.
 */
int
slsrest_proc(struct proc *p, uint64_t daemon, struct slsproc *slsproc,
    struct slsrest_data *restdata)
{
	int error;

	PROC_LOCK(p);
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
	KASSERT(!SESS_LEADER(p), ("process is a session leader"));
	/* Check if we were the original session leader. */
	if (slsproc->pid == slsproc->sid) {
		error = slsrest_session(slsproc, restdata);
		if (error != 0)
			goto out;
	} else if (slsproc->pid == slsproc->pgid) {
		error = slsrest_pgrp(slsproc, restdata, daemon);
		if (error != 0)
			goto out;
	}

	/* Wake up any processes waiting for a session, group, or parent. */
	mtx_lock(&restdata->procmtx);
	cv_broadcast(&restdata->proccv);
	mtx_unlock(&restdata->procmtx);

	/*
	 * Wait for the session, group, and parent of the process. We can sleep,
	 * so release the process lock.
	 */
	PROC_UNLOCK(p);
	slsrest_procwait(slsproc, restdata);
	PROC_LOCK(p);

	/* XXX Leaderless pgroups are not being migrated to a new pgroup. */
	/* If we need to enter a pgroup, look it up in the tables and do it
	 * here. */
	if ((slsproc->pid != slsproc->pgid) && (slsproc->pgrpwait != 0)) {
		error = slsrest_attach_pgrp(slsproc->pgid, restdata);
		if (error != 0)
			goto out;
	} else if ((slsproc->pid != slsproc->sid) &&
	    (slsproc->pid == slsproc->pgid) && (slsproc->sid != 0)) {
		error = slsrest_attach_sess(slsproc->sid, restdata);
		if (error != 0)
			goto out;
	}

	/*
	 * Reparent the process to the original parent, if it exists.
	 * Otherwise the parent remains the original sls_restore process.
	 */
	error = slsrest_proc_reparent(slsproc, restdata, daemon);
	if (error != 0)
		goto out;

	error = slsrest_proc_signals(slsproc, restdata);
	if (error != 0)
		goto out;

	error = slsrest_proc_textvp(slsproc, restdata);
	if (error != 0)
		goto out;

	/* Restore the standard syscall vector.*/
	/*
	 * XXX Allow for arbitrary syscall vectors.
	 * The correct way would be to find out what kind of vector
	 * this is at checkpoint time, encoding it as a value in the
	 * process, then doing the opposite at restore time (finding
	 * the correct vector by using the value as an index in a table).
	 */
	p->p_sysent = &elf64_freebsd_sysvec;

out:
	PROC_UNLOCK(p);

	return (error);
}
