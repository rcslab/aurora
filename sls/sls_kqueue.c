#include <sys/param.h>
#include <sys/selinfo.h>
#include <sys/capsicum.h>
#include <sys/domain.h>
#include <sys/endian.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/ktr.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

#include <machine/param.h>

/*
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <slos.h>
#include <sls_data.h>

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"

/*
 * All kqueues belong to exactly one file table, and have a backpointer to it.
 * Remove the kqueue from its file table and fix the backpointers.
 */
void
slskq_detach(struct kqueue *kq)
{
	struct filedesc *fdp = kq->kq_fdp;

	FILEDESC_XLOCK(fdp);
	TAILQ_REMOVE(&fdp->fd_kqlist, kq, kq_list);
	kq->kq_fdp = NULL;
	FILEDESC_XUNLOCK(fdp);
}

void
slskq_attach_locked(struct proc *p, struct kqueue *kq)
{
	struct filedesc *fdp = p->p_fd;

	FILEDESC_LOCK_ASSERT(fdp);

	TAILQ_INSERT_HEAD(&fdp->fd_kqlist, kq, kq_list);
	kq->kq_fdp = fdp;
}

/*
 * All kqueues belong to exactly one file table, and have a backpointer to it.
 * Fixup the references and backpointers of the process' file table and kqueue.
 */
void
slskq_attach(struct proc *p, struct kqueue *kq)
{
	struct filedesc *fdp = p->p_fd;

	FILEDESC_XLOCK(fdp);
	slskq_attach_locked(p, kq);
	FILEDESC_XUNLOCK(fdp);
}

static int
slskq_checkpoint_knote(struct knote *kn, struct sbuf *sb)
{
	struct slsknote slskn;
	int error;

	KASSERT(kn->kn_influx == 0, ("knote in flux while checkpointing"));
	/*
	KASSERT((kn->kn_status & (KN_DETACHED | KN_MARKER | KN_SCAN)) == 0,
		("illegal knote state %x", kn->kn_status));
		*/

	if ((kn->kn_status & (KN_MARKER | KN_DETACHED)) != 0)
		return (0);

	/*
	 * The AIO subsystem stores kernel pointers in knotes' private data,
	 * which we obviously can't do anything about. Fail when we come across
	 * this situation.
	 */
	KASSERT((kn->kn_kevent.filter != EVFILT_AIO),
	    ("unhandled AIO filter detected"));

	/* XXX Check the kevent's flags in case there is an illegal operation in
	 * progress. */
	DEBUG2("Checkpointing (%lx, %d)", kn->kn_kevent.ident,
	    kn->kn_kevent.filter);

	/* Write each kevent to the sbuf. */
	/* Get all relevant fields, mostly the identifier a*/
	slskn.magic = SLSKNOTE_ID;
	slskn.slsid = (uint64_t)kn;
	slskn.kn_status = kn->kn_status;
	slskn.kn_kevent = kn->kn_kevent;
	slskn.kn_sfflags = kn->kn_sfflags;
	slskn.kn_sdata = kn->kn_sdata;

	error = sbuf_bcat(sb, (void *)&slskn, sizeof(slskn));
	if (error != 0)
		return (error);

	return (0);
}

/*
 * Checkpoint a kqueue and all of its associated knotes.
 */
static int
slskq_checkpoint_kqueue(struct proc *p, struct kqueue *kq, struct sbuf *sb)
{
	struct slskqueue slskq;
	struct knote *kn;
	int error, i;

	/*
	 * The kqueue structure is empty, we are only using it as a header to
	 * the array of knotes that succeed it.
	 */
	slskq.magic = SLSKQUEUE_ID;
	slskq.slsid = (uint64_t)kq;

	/* Write the kqueue itself to the sbuf. */
	error = sbuf_bcat(sb, (void *)&slskq, sizeof(slskq));
	if (error != 0)
		return (error);

	/*
	 * For the SLS, all kqueue states are either irrelevant or illegal.
	 * Check for the illegal ones.
	 *
	 * XXX KQ_ASYNC and KQ_SEL are dodgy, but don't seem to be a problem.
	 */
	KASSERT((kq->kq_state & (KQ_FLUXWAIT)) == 0,
	    ("illegal kqueue state %x", kq->kq_state));

	/* Get all knotes in the dynamic array. */
	for (i = 0; i < kq->kq_knlistsize; i++) {
		SLIST_FOREACH (kn, &kq->kq_knlist[i], kn_link) {
			error = slskq_checkpoint_knote(kn, sb);
			if (error != 0)
				return (error);
		}
	}

	/* Do the exact same thing for the hashtable. */
	for (i = 0; i < kq->kq_knhashmask; i++) {
		SLIST_FOREACH (kn, &kq->kq_knhash[i], kn_link) {
			error = slskq_checkpoint_knote(kn, sb);
			if (error != 0)
				return (error);
		}
	}

	/*
	 * We don't care about the pending knotes queue because we can deduce
	 * which knotes are active from their state. All knotes are accessible
	 * from the list and the hashtable, because we don't remove them from
	 * there to attach them to the pending list.
	 */

	return (0);
}

int
slskq_checkpoint(
    struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	struct kqueue *kq;
	int error;

	/* Acquire the kqueue for reading. */
	error = kqueue_acquire(fp, &kq);
	if (error != 0)
		return (error);

	info->backer = (uint64_t)kq;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *)info, sizeof(*info));
	if (error != 0)
		return (error);

	error = slskq_checkpoint_kqueue(p, kq, sb);
	if (error != 0)
		goto out;

	kqueue_release(kq, 0);

	return (0);

out:
	kqueue_release(kq, 0);
	return (error);
}

/*
 * Restore a kqueue for a process. Kqueues are an exception among
 * entities using the file abstraction, in that they need the rest
 * of the files in the process' table to be restored before we can
 * fully restore it. More specifically, while we create the kqueue
 * here, in order for it to be inserted to the proper place in the
 * file table, we do not populate it with kevents. This is because
 * each kevent targets an fd, and so it can't be done while the
 * files are being restored. We wait until that step is done before
 * we fully populate the kqueues with their data.
 */
int
slskq_restore_kqueue(struct slskqueue *slskq, int *fdp)
{
	struct proc *p = curproc;
	struct kqueue *kq;
	int error;
	int fd;

	/* Create a kqueue for the process. */
	error = kern_kqueue(curthread, 0, NULL);
	if (error != 0)
		return (error);

	fd = curthread->td_retval[0];
	kq = FDTOFP(p, fd)->f_data;

	/*
	 * Right now we are creating the kqueue outside the file table in which
	 * it will ultimately end up. We need to remove it from its current one,
	 * we'll attach it to the currect table when that is created.
	 */
	slskq_detach(kq);

	/* Grab the open file and pass it to the caller. */
	*fdp = fd;

	return (0);
}

/*
 * Find a given knote specified by (kqueue, ident, filter).
 */
static struct knote *
slskq_knfind(struct kqueue *kq, __uintptr_t ident, short filter)
{
	struct klist *list;
	struct knote *kn;

	/* Traversal same as in kqueue_register(). */
	if (ident < kq->kq_knlistsize) {
		SLIST_FOREACH (kn, &kq->kq_knlist[ident], kn_link) {
			if (filter == kn->kn_filter)
				return (kn);
		}
	}

	if (kq->kq_knhashmask != 0) {
		list =
		    &kq->kq_knhash[KN_HASH((u_long)ident, kq->kq_knhashmask)];
		SLIST_FOREACH (kn, list, kn_link) {
			if (ident == kn->kn_id && filter == kn->kn_filter)
				return (kn);
		}
	}

	return (NULL);
}

/*
 * Register a list of kevents into the kqueue.
 */
static int
slskq_register(int fd, struct kqueue *kq, slsset *slskns)
{
	struct slskv_iter iter;
	struct slsknote *slskn;
	struct kevent kev;
	int error;

	KVSET_FOREACH(slskns, iter, slskn)
	{
		kev = slskn->kn_kevent;
		DEBUG2("Registering knote (%lx, %d)", kev.ident, kev.filter);
		/*
		 * We need to modify the action flags so that the call to
		 * kqfd_register() does exactly what we want: We want the knote
		 * to be inserted, but not triggered (we enqueue it manually if
		 * needed). The way to do this is by adding the kevent in a
		 * disabled state. We'll restore the right action flags later.
		 */
		kev.flags = EV_ADD | EV_DISABLE;
		error = kqueue_register(kq, &kev, curthread, M_WAITOK);
		if (error != 0) {
			SLS_DBG("(BUG) Error %d by restoring knote for fd %d\n",
			    error, fd);
			SLS_DBG("(BUG) Ident %lx Filter %d\n", kev.ident,
			    kev.filter);
		}
		/* XXX See if we can handle/return the error. */
	}

	return (0);
}

/*
 * XXX Temporary hack for connnected sockets. We can't restore such sockets, so
 * any thread doing kevent() on them should see an EOF event. Manually traverse
 * the file descriptor table and set the knotes as EOF.
 */
static void
slskq_sockhack(struct proc *p, struct kqueue *kq)
{
	struct filedesc *fdp = p->p_fd;
	struct socket *so;
	struct knote *kn;
	struct file *fp;
	int fd;

	/* Scan the filetable for active files. */
	for (fd = 0; fd <= fdp->fd_lastfile; fd++) {
		if (!fdisused(fdp, fd))
			continue;

		/* Filter out everything but connected inet sockets. */
		fp = FDTOFP(p, fd);
		if (fp->f_type != DTYPE_SOCKET)
			continue;

		so = (struct socket *)fp->f_data;
		if (so->so_proto->pr_domain->dom_family != AF_INET)
			continue;

		if ((so->so_options & SO_ACCEPTCONN) != 0)
			continue;

		/*
		 * Find all knotes for the identifier, set them as EOF.
		 * The knote identifier is the fd of the file.
		 */
		SLIST_FOREACH (kn, &kq->kq_knlist[fd], kn_link) {
			DEBUG4("Restoring knote ident = %d, filter = %d "
			       "flags = 0x%x status = 0x%x",
			    kn->kn_id, kn->kn_filter, kn->kn_flags,
			    kn->kn_status);

			if ((kn->kn_status & KN_QUEUED) == 0) {
				kn->kn_status |= (KN_ACTIVE | KN_QUEUED);
				TAILQ_INSERT_TAIL(&kq->kq_head, kn, kn_tqe);
				kq->kq_count += 1;
			}
			kn->kn_flags |= EV_ERROR;
			kn->kn_data = ECONNRESET;
		}
	}
}

/*
 * Restore the kevents of a kqueue. This is done after all files
 * have been restored, since we need the fds to be backed before
 * we attach the relevant kevent to the kqueue.
 */
static int
slskq_restore_knotes(int fd, slsset *slskns)
{
	struct thread *td = curthread;
	struct slsknote *slskn;
	struct kevent *kev;
	struct kqueue *kq;
	struct knote *kn;
	struct file *fp;
	int error;

	/* Get the underlying kqueue, as in kqfd_register(). */

	/* First get the file pointer out of the table. */
	error = fget(td, fd, &cap_no_rights, &fp);
	if (error != 0)
		return (error);

	/* Then unpack the kqueue out of the descriptor. */
	error = kqueue_acquire(fp, &kq);
	if (error != 0)
		goto noacquire;

	/* For each slskq, create a kevent and register it. */
	error = slskq_register(fd, kq, slskns);
	if (error != 0)
		goto noacquire;

	/*
	 * We assume that knotes won't be messed with by external entities until
	 * fully initialized below, thanks to the EV_DISABLE flag. We also
	 * assume the kqueue won't be modified externally, since the process is
	 * not running.
	 */

	KQ_LOCK(kq);
	/* Traverse the kqueue, fixing up each knote as we go. */
	KVSET_FOREACH_POP(slskns, slskn)
	{
		/* First try to find the knote in the fd-related array. */
		kev = &slskn->kn_kevent;

		/*
		 * Find the right knote. Some files like non-listening IP
		 * sockets and IPv6 sockets of all kinds do not get restored, so
		 * a knote might be missing.
		 */
		kn = slskq_knfind(kq, kev->ident, kev->filter);
		if (kn == NULL) {
			SLS_DBG("Missing knote (%lx, %x)\n", kev->ident,
			    kev->filter);
			continue;
		}

		KASSERT(kn != NULL,
		    ("Missing knote (fd, id, ident) = (%d, %lx ,%d))", fd,
			kev->ident, kev->filter));

		SLS_DBG("Restoring (%lx, %d)\n", kev->ident, kev->filter);
		/*
		 * We are holding the kqueue lock here, so we do not need to
		 * mark the knote as being in flux while modifying.
		 */

		/*
		 * If the knote is supposed to be active, put it in the active
		 * list. Avoid kqueue_enqueue() to avoid waking up the kqueue.
		 * Any pending wakeups to at checkpoint time are lost, but we
		 * restart all syscalls, so it doesn't matter.
		 */
		if ((slskn->kn_status & KN_QUEUED) != 0) {
			TAILQ_INSERT_TAIL(&kq->kq_head, kn, kn_tqe);
			kq->kq_count += 1;
		}

		kn->kn_status = slskn->kn_status;
		/* This restores the action flags, among other fields. */
		kn->kn_kevent = slskn->kn_kevent;
		kn->kn_sfflags = slskn->kn_sfflags;
		kn->kn_sdata = slskn->kn_sdata;
		DEBUG4("Restoring knote ident = %d, filter = %d"
		       "flags = 0x%x status = 0x%x",
		    kn->kn_id, kn->kn_filter, kn->kn_flags, kn->kn_status);
	}

	/* XXX Temporary hack. Send an EOF event to any non-restored sockets. */
	slskq_sockhack(curproc, kq);

	KQ_UNLOCK(kq);

	kqueue_release(kq, 0);

noacquire:
	fdrop(fp, td);

	return (0);
}

/* Restore the knotes to the already restored kqueues. */
int
slskq_restore_knotes_all(struct proc *p, struct slskv_table *kevtable)
{
	struct file *fp;
	slsset *kevset;
	int error;
	int fd;

	for (fd = 0; fd <= p->p_fd->fd_lastfile; fd++) {
		if (!fdisused(p->p_fd, fd))
			continue;

		/* We only want kqueue-backed open files. */
		fp = FDTOFP(p, fd);
		if (fp->f_type != DTYPE_KQUEUE)
			continue;

		/* If we're a kqueue, we _have_ to have a set, even if empty. */
		error = slskv_find(
		    kevtable, (uint64_t)fp->f_data, (uintptr_t *)&kevset);
		if (error != 0)
			return (error);

		error = slskq_restore_knotes(fd, kevset);
		if (error != 0)
			return (error);
	}

	return (0);
}
