#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/selinfo.h>
#include <sys/capsicum.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/event.h>
#include <sys/eventvar.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>
#include <sys/time.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/unistd.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <machine/param.h>
#include <machine/reg.h>
#include <machine/vmparam.h>

#include <netinet/in.h>

#include <slos.h>
#include <slos_inode.h>

#include "debug.h"
#include "sls_data.h"
#include "sls_file.h"
#include "sls_internal.h"
#include "sls_ioctl.h"
#include "sls_kv.h"
#include "sls_load.h"
#include "sls_partition.h"
#include "sls_proc.h"
#include "sls_sysv.h"
#include "sls_table.h"
#include "sls_vm.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"
#include "sls_vnode.h"
#include "sysv_internal.h"

SDT_PROBE_DEFINE1(sls, , sls_rest, , "char *");
SDT_PROBE_DEFINE1(sls, , slsrest_metadata, , "char *");
SDT_PROBE_DEFINE0(sls, , slsrest_start, );
SDT_PROBE_DEFINE0(sls, , slsrest_end, );

static uma_zone_t slsrest_zone;

static int
slsrest_zone_ctor(void *mem, int size, void *args __unused, int flags __unused)
{
	struct slsrest_data *restdata = (struct slsrest_data *)mem;

	restdata->proctds = 0;

	return (0);
}

static void
slsrest_zone_dtor(void *mem, int size, void *args __unused)
{
	struct slsrest_data *restdata = (struct slsrest_data *)mem;
	struct mbuf *m, *headm;
	struct session *sess;
	struct kqueue *kq;
	uint64_t slskn;
	slsset *kevset;
	void *slskev;
	uint64_t slsid;
	struct vnode *vp;
	vm_object_t obj;
	struct file *fp;
	struct proc *p;

	KV_FOREACH_POP(restdata->fptable, slsid, fp)
	{
		/*
		 * Kqueues in the file table do not have an associated file
		 * table, contrary to what kqueue_close() assumes.
		 * Attach the kqueue to this process' file table before
		 * destroying it.
		 */
		if (fp->f_type == DTYPE_KQUEUE) {
			kq = (struct kqueue *)fp->f_data;
			if (kq->kq_fdp == NULL)
				slsrest_kqattach(curproc, kq);
		}

		fdrop(fp, curthread);
	}

	KV_FOREACH_POP(restdata->objtable, slsid, obj)
	{
		if (obj != NULL)
			vm_object_deallocate(obj);
	}

	KV_FOREACH_POP(restdata->kevtable, slskn, kevset)
	{
		KVSET_FOREACH_POP(kevset, slskev)
		free(slskev, M_SLSMM);

		slsset_destroy(kevset);
	}
	KV_FOREACH_POP(restdata->mbuftable, slsid, headm)
	{
		while (headm != NULL) {
			m = headm;
			headm = headm->m_nextpkt;
			m_free(m);
		}
	}

	KVSET_FOREACH_POP(restdata->vntable, vp)
	vrele(vp);

	KV_FOREACH_POP(restdata->sesstable, slsid, sess)
	sess_release(sess);

	/*
	 * We do not clean up the process groups because each one belongs to at
	 * least one process (the one that created it), and will be cleaned up
	 * automatically when that process exits.
	 */

	KV_FOREACH_POP(restdata->proctable, slsid, p)
	PRELE(p);
}

static int
slsrest_zone_init(void *mem, int size, int flags __unused)
{
	struct slsrest_data *restdata = (struct slsrest_data *)mem;
	;
	int error;

	mtx_init(&restdata->procmtx, "SLS proc mutex", NULL, MTX_DEF);
	cv_init(&restdata->proccv, "SLS proc cv");
	restdata->proctds = -1;

	/* Initialize the necessary tables. */
	error = slskv_create(&restdata->objtable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->proctable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->fptable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->kevtable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->pgidtable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->sesstable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->mbuftable);
	if (error != 0)
		goto error;

	error = slskv_create(&restdata->vntable);
	if (error != 0)
		goto error;

	return (0);

error:
	if (restdata->vntable != NULL)
		slskv_destroy(restdata->vntable);

	if (restdata->mbuftable != NULL)
		slskv_destroy(restdata->mbuftable);

	if (restdata->sesstable != NULL)
		slskv_destroy(restdata->sesstable);

	if (restdata->pgidtable != NULL)
		slskv_destroy(restdata->pgidtable);

	if (restdata->kevtable != NULL)
		slskv_destroy(restdata->kevtable);

	if (restdata->fptable != NULL)
		slskv_destroy(restdata->fptable);

	if (restdata->proctable != NULL)
		slskv_destroy(restdata->proctable);

	if (restdata->objtable != NULL)
		slskv_destroy(restdata->objtable);

	return (ENOMEM);
}

static void
slsrest_zone_fini(void *mem, int size)
{
	struct slsrest_data *restdata = (struct slsrest_data *)mem;

	slskv_destroy(restdata->vntable);
	slskv_destroy(restdata->mbuftable);
	slskv_destroy(restdata->sesstable);
	slskv_destroy(restdata->pgidtable);
	slskv_destroy(restdata->objtable);
	slskv_destroy(restdata->proctable);
	slskv_destroy(restdata->fptable);
	slskv_destroy(restdata->kevtable);

	cv_destroy(&restdata->proccv);
	mtx_destroy(&restdata->procmtx);
}

int
slsrest_zoneinit(void)
{
	int error;

	slsrest_zone = uma_zcreate("slsrest", sizeof(struct slsrest_data),
	    slsrest_zone_ctor, slsrest_zone_dtor, slsrest_zone_init,
	    slsrest_zone_fini, UMA_ALIGNOF(struct slsrest_data), 0);
	if (slsrest_zone == NULL)
		return (ENOMEM);

	error = sls_zonewarm(slsrest_zone);
	if (error != 0)
		printf("WARNING: Zone slsrest not warmed up\n");

	return (0);
}

void
slsrest_zonefini(void)
{
	if (slsrest_zone != NULL)
		uma_zdestroy(slsrest_zone);
}

static int
slsrest_dothread(struct proc *p, char **bufp, size_t *buflenp)
{
	struct slsthread slsthread;
	int error;

	error = slsload_thread(&slsthread, bufp, buflenp);
	if (error != 0) {
		DEBUG1("Error in slsload_thread %d", error);
		return (error);
	}

	error = slsrest_thread(p, &slsthread);
	if (error != 0) {
		DEBUG1("Error in slsrest_thread %d", error);
		return (error);
	}

	return (0);
}

static int
slsrest_doproc(struct proc *p, uint64_t daemon, char **bufp, size_t *buflenp,
    struct slsrest_data *restdata)
{
	struct slsproc slsproc;
	int error, i;

	error = slsload_proc(&slsproc, bufp, buflenp);
	if (error != 0)
		return (error);

	/*
	 * Save the old process - new process pairs.
	 */
	DEBUG("Adding id to slskv table");
	PHOLD(p);
	error = slskv_add(
	    restdata->proctable, (uint64_t)slsproc.slsid, (uintptr_t)p);
	if (error != 0) {
		PRELE(p);
		SLS_DBG("Error: Could not add process %p to table\n", p);
	}

	DEBUG("Attempting restore of proc");
	error = slsrest_proc(p, daemon, &slsproc, restdata);
	if (error != 0)
		return (error);

	for (i = 0; i < slsproc.nthreads; i++) {
		DEBUG("Attempting restore of thread");
		error = slsrest_dothread(p, bufp, buflenp);
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slsrest_dovnode(struct slsrest_data *restdata, char **bufp, size_t *buflenp)
{
	struct slsvnode slsvnode;
	int error;

	error = slsload_vnode(&slsvnode, bufp, buflenp);
	if (error != 0) {
		DEBUG1("Error in slsload_vnode %d", error);
		return (error);
	}

	error = slsrest_vnode(&slsvnode, restdata);
	if (error != 0) {
		DEBUG1("Error in slsrest_vnode %d", error);
		return (error);
	}

	return (0);
}

static int
slsrest_dofile(struct slsrest_data *restdata, char *buf, size_t buflen)
{
	struct slsfile slsfile;
	void *data;
	int error;

	error = slsload_file(&slsfile, &data, &buf, &buflen);
	if (error != 0)
		return (error);

	error = slsrest_file(data, &slsfile, restdata);

	switch (slsfile.type) {
	case DTYPE_KQUEUE:
	case DTYPE_VNODE:
	case DTYPE_FIFO:
		/* Nothing to clean up. */
		break;

	case DTYPE_SHM:
	case DTYPE_PIPE:
	case DTYPE_SOCKET:
	case DTYPE_PTS:
		free(data, M_SLSMM);
		break;

	default:
		panic("Tried to restore file type %d", slsfile.type);
	}

	return (error);
}

static int
slsrest_dofiledesc(
    struct proc *p, char **bufp, size_t *buflenp, struct slsrest_data *restdata)
{
	int error = 0;
	struct slsfiledesc slsfiledesc;
	struct slskv_table *fdtable;

	error = slsload_filedesc(&slsfiledesc, bufp, buflenp, &fdtable);
	if (error != 0)
		return (error);

	error = slsrest_filedesc(p, &slsfiledesc, fdtable, restdata);
	slskv_destroy(fdtable);

	return (error);
}

static int
slsrest_dovmspace(
    struct proc *p, char **bufp, size_t *buflenp, struct slsrest_data *restdata)
{
	struct slsvmentry slsvmentry;
	struct slsvmspace slsvmspace;
	struct shmmap_state *shmstate = NULL;
	vm_map_t map;
	int error, i;

	/*
	 * Restore the VM space and its map. We need to read the state even if
	 * we are restoring from a cached vmspace.
	 */
	error = slsload_vmspace(&slsvmspace, &shmstate, bufp, buflenp);
	if (error != 0)
		return (error);

	error = slsrest_vmspace(p, &slsvmspace, shmstate);
	if (error != 0) {
		free(shmstate, M_SHM);
		return (error);
	}

	map = &p->p_vmspace->vm_map;

	/* Create the individual VM entries. */
	for (i = 0; i < slsvmspace.nentries; i++) {
		error = slsload_vmentry(&slsvmentry, bufp, buflenp);
		if (error != 0)
			return (error);

		error = slsrest_vmentry(map, &slsvmentry, restdata);
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slsrest_dosysvshm(char *buf, size_t bufsize, struct slskv_table *objtable)
{
	struct slssysvshm slssysvshm;
	size_t numsegs;
	int error, i;

	/*
	 * The buffer only has segments in it, so its size must
	 * be a multiple of the struct we read into.
	 */
	if (((bufsize % sizeof(slssysvshm)) != 0))
		return (EINVAL);

	/* Read each segment in, restoring metadata and attaching to the object.
	 */
	numsegs = bufsize / sizeof(slssysvshm);
	for (i = 0; i < numsegs; i++) {
		error = slsload_sysvshm(&slssysvshm, &buf, &bufsize);
		if (error != 0)
			return (error);

		error = slsrest_sysvshm(&slssysvshm, objtable);
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slsrest_dosockbuf(char *buf, size_t bufsize, struct slskv_table *table)
{
	struct mbuf *m, *errm;
	uint64_t sbid;
	int error;

	panic("slsrest_dosockbuf called\n");
	error = slsload_sockbuf(&m, &sbid, &buf, &bufsize);
	if (error != 0)
		return (error);

	error = slskv_add(table, sbid, (uintptr_t)m);
	if (error != 0) {
		errm = m;
		while (errm != NULL) {
			m = errm;
			errm = errm->m_nextpkt;
			m_free(m);
		}

		return (error);
	}

	return (0);
}

/* Restore the knotes to the already restored kqueues. */
static int
slsrest_doknotes(struct proc *p, struct slskv_table *kevtable)
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

		error = slsrest_knotes(fd, kevset);
		if (error != 0)
			return (error);
	}

	return (0);
}

/*
 * Release all master terminals references from the restoring thread, causing
 * the destruction of any PTSes that are not referenced by restore processes.
 * Needed by slsrest_ttyfixup().
 */
static void
slsrest_ttyrelease(struct slsrest_data *restdata)
{
	struct slskv_iter iter;
	struct file *fp;
	uint64_t slsid;

	/* We can remove elements while iterating. */
	KV_FOREACH(restdata->fptable, iter, slsid, fp)
	{
		if (fp->f_type == DTYPE_PTS) {
			slskv_del_unlocked(restdata->fptable, slsid);
			fdrop(fp, curthread);
		}
	}
}

/*
 * Remove /dev/pts devices without accompanying struct ttys.
 */
static int
slsrest_ttyfixup(struct proc *p)
{
	struct thread *td = curthread;
	struct file *fp, *pttyfp;
	char *ttyname, *path;
	struct tty *tp;
	int error, ret;
	int fd;

	PROC_LOCK_ASSERT(p, MA_NOTOWNED);

	/* Construct the controlling terminal's name. */
	ttyname = __DECONST(char *, tty_devname(p->p_session->s_ttyp));
	path = malloc(2 * PATH_MAX, M_SLSMM, M_WAITOK);
	strlcpy(path, "/dev/", sizeof("/dev/"));
	strlcat(path, ttyname, PATH_MAX);

	/* Open the device and grab the file. */
	error = kern_openat(td, AT_FDCWD, path, UIO_SYSSPACE, O_RDWR, S_IRWXU);
	free(path, M_SLSMM);
	if (error != 0)
		return (error);

	fd = curthread->td_retval[0];
	pttyfp = FDTOFP(p, fd);
	ret = fhold(pttyfp);
	error = kern_close(td, fd);
	if (error != 0)
		DEBUG1("Error %d when closing TTY\n", error);
	if (ret == 0)
		return (EBADF);

	/*
	 * CAREFUL: We are using our controlling terminal, so we assume we have
	 * one.
	 */

	FILEDESC_XLOCK(p->p_fd);
	for (fd = 0; fd <= p->p_fd->fd_lastfile; fd++) {
		/* Only care about used descriptor table slots. */
		if (!fdisused(p->p_fd, fd))
			continue;

		/* If we're a tty, see if the master side is still there.  */
		fp = FDTOFP(p, fd);
		if ((fp->f_type == DTYPE_VNODE) &&
		    (fp->f_vnode->v_type == VCHR) &&
		    (fp->f_vnode->v_rdev->si_devsw->d_flags & D_TTY) != 0) {

			tp = fp->f_vnode->v_rdev->si_drv1;
			if (!tty_gone(tp))
				continue;

			/*
			 * The master is gone, replace the file with the
			 * parent's tty device and adjust hold counts.
			 */
			fdrop(fp, curthread);

			if (!fhold(pttyfp))
				goto error;

			_finstall(p->p_fd, pttyfp, fd, O_CLOEXEC, NULL);
		}
	}
	FILEDESC_XUNLOCK(p->p_fd);

	fdrop(pttyfp, td);

	return (0);

error:
	FILEDESC_XUNLOCK(p->p_fd);
	return (EBADF);
}

static int
slsrest_metrsocket(struct proc *p, struct slsrest_data *restdata)
{
	struct slsmetr *slsmetr = &restdata->slsmetr;
	struct thread *td = curthread;
	struct proc *metrproc;
	int error;
	int fd;

	/* Check if the process is the partition's Metropolis process. */
	error = slskv_find(
	    restdata->proctable, slsmetr->slsmetr_proc, (uintptr_t *)&metrproc);
	if (error != 0)
		return (0);

	if (p != metrproc)
		return (0);

	FILEDESC_XLOCK(p->p_fd);
	/* Attach the file into the file table. */
	error = fdalloc(td, AT_FDCWD, &fd);
	if (error != 0) {
		FILEDESC_XUNLOCK(p->p_fd);
		return (error);
	}

	_finstall(p->p_fd, slsmetr->slsmetr_sockfp, fd, O_CLOEXEC, NULL);
	FILEDESC_XUNLOCK(p->p_fd);

	if (slsmetr->slsmetr_addrsa != NULL) {
		error = copyout(slsmetr->slsmetr_sa, slsmetr->slsmetr_addrsa,
		    slsmetr->slsmetr_namelen);
		if (error != 0)
			printf("Could not write out socket address\n");

		copyout(&slsmetr->slsmetr_namelen, slsmetr->slsmetr_addrlen,
		    sizeof(slsmetr->slsmetr_namelen));
		if (error != 0)
			printf("Could not write out socket address length\n");
	}

	td->td_retval[0] = fd;

	return (0);
}

static int
slsrest_metrfixup(struct proc *p, struct slsrest_data *restdata)
{
	struct thread *td;
	int error;

	/* Attach the socket into this process. */
	error = slsrest_metrsocket(p, restdata);
	if (error != 0)
		return (error);

	/* Find the thread that originally did the call. */
	FOREACH_THREAD_IN_PROC (p, td) {
		if (restdata->slsmetr.slsmetr_td == td->td_oldtid) {
			/* Forward the fd to it. */
			td->td_retval[0] = curthread->td_retval[0];
			(p->p_sysent->sv_set_syscall_retval)(td, 0);
		}
	}

	return (0);
}

/* Struct used to set the arguments after forking. */
struct slsrest_metadata_args {
	char *buf;
	size_t buflen;
	uint64_t daemon;
	uint64_t rest_stopped;
	struct slsrest_data *restdata;
};

/*
 * Restore a process' local data (threads, VM map, file descriptor table).
 */
static void __attribute__((noinline)) slsrest_metadata(void *args)
{
	struct slsrest_data *restdata;
	struct proc *p = curproc;
	uint64_t rest_stopped;
	uint64_t daemon;
	size_t buflen;
	int error;
	char *buf;

	/*
	 * Transfer the arguments to the stack and free the
	 * struct we used to carry them to the new process.
	 */

	buf = ((struct slsrest_metadata_args *)args)->buf;
	buflen = ((struct slsrest_metadata_args *)args)->buflen;
	daemon = ((struct slsrest_metadata_args *)args)->daemon;
	rest_stopped = ((struct slsrest_metadata_args *)args)->rest_stopped;
	restdata = ((struct slsrest_metadata_args *)args)->restdata;

	free(args, M_SLSMM);

	/* We always work on the current process. */
	SLS_LOCK();
	PROC_LOCK(p);
	thread_single(p, SINGLE_BOUNDARY);

	/* Insert new process into Aurora. */
	p->p_auroid = restdata->slsp->slsp_oid;
	slsm_procadd(p);
	SLS_UNLOCK();

	SDT_PROBE1(sls, , slsrest_metadata, , "Single threading");

	PROC_UNLOCK(p);

	error = slsrest_dovmspace(p, &buf, &buflen, restdata);
	if (error != 0)
		goto error;

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring vm state");
	/*
	 * Restore CPU state, file state, and memory
	 * state, parsing the buffer at each step.
	 */
	error = slsrest_doproc(p, daemon, &buf, &buflen, restdata);
	if (error != 0)
		goto error;

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring process state");
	error = slsrest_dofiledesc(p, &buf, &buflen, restdata);
	if (error != 0)
		goto error;

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring file table");
	error = slsrest_doknotes(p, restdata->kevtable);
	if (error != 0)
		goto error;

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring kqueues");

	if (restdata->slsmetr.slsmetr_proc != 0)
		slsrest_metrfixup(p, restdata);

	SDT_PROBE1(sls, , slsrest_metadata, , "Metropolis fixup");

	/* We're done restoring. If we are the last, notify the parent. */
	mtx_lock(&restdata->procmtx);

	/*
	 * The value of proctds can only be -1 after all restored threads are
	 * stuck waiting for the main one. This thread isn't stuck yet, so the
	 * value can't be 0 here.
	 */
	KASSERT(restdata->proctds > 0,
	    ("invalid proctds value %d", restdata->proctds));
	restdata->proctds -= 1;

	/* Wake up the main thread if it's the only one waiting. */
	if (restdata->proctds == 0)
		cv_broadcast(&restdata->proccv);

	/*
	 * Sleep until all sessions and controlling terminals are restored.
	 * The value -1 for proctds is used to denote that the main restore
	 * thread is done calling slsrest_ttyrelease.
	 */
	while (restdata->proctds >= 0)
		cv_wait(&restdata->proccv, &restdata->procmtx);
	mtx_unlock(&restdata->procmtx);

	SDT_PROBE1(sls, , slsrest_metadata, , "Process wakeup");

	DEBUG("SLS Restore ttyfixup");

	/* This must take place after slsrest_ttyrelease in the main thread. */
	error = slsrest_ttyfixup(p);
	if (error != 0)
		SLS_DBG("tty_fixup failed with %d\n", error);

	/*
	 * If the original applications are running on a terminal,
	 * then the master side is not included in the checkpoint. Since
	 * in that case it doesn't make much sense restoring the terminal,
	 * we instead pass the original restore process' terminal as an
	 * argument.
	 */

	SDT_PROBE1(sls, , slsrest_metadata, , "Fixing up the tty");
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	if (rest_stopped == 1)
		kern_psignal(p, SIGSTOP);
	PROC_UNLOCK(p);
	SDT_PROBE0(sls, , slsrest_end, );

	/* Restore the process in a stopped state if needed. */
	SDT_PROBE1(sls, , slsrest_metadata, , "Ending single threading");
	kthread_exit();

	panic("Having the kthread exit failed");

	PROC_UNLOCK(p);

	return;

error:
	/*
	 * Get the process unstuck from the boundary. The exit1() call will
	 * single thread again, this time in order to exit.
	 */
	PROC_LOCK(p);
	thread_single_end(p, SINGLE_BOUNDARY);
	PROC_UNLOCK(p);

	DEBUG1("Error %d while restoring process\n", error);
	mtx_lock(&restdata->procmtx);
	KASSERT(restdata->proctds > 0,
	    ("invalid proctds value %d", restdata->proctds));

	restdata->proctds -= 1;
	if (restdata->proctds == 0)
		cv_broadcast(&restdata->proccv);
	mtx_unlock(&restdata->procmtx);

	exit1(curthread, error, 0);
}

static int
slsrest_fork(uint64_t daemon, uint64_t rest_stopped, char *buf, size_t buflen,
    struct slsrest_data *restdata)
{
	struct fork_req fr;
	struct thread *td;
	struct proc *p2;
	struct slsrest_metadata_args *args;
	int error;

	bzero(&fr, sizeof(fr));

	/*
	 * Copy the file table to the new process,
	 * and do not schedule it just yet.
	 */
	fr.fr_flags = RFFDG | RFPROC | RFSTOPPED | RFMEM;
	fr.fr_procp = &p2;

	error = fork1(curthread, &fr);
	if (error != 0)
		return (error);

	/* The code below is executed only by the original thread. */
	args = malloc(sizeof(*args), M_SLSMM, M_WAITOK);
	args->buf = buf;
	args->daemon = daemon;
	args->rest_stopped = rest_stopped;
	args->buflen = buflen;
	args->restdata = restdata;

	/* Note down the fact that one more process is being restored. */
	mtx_lock(&restdata->procmtx);
	restdata->proctds += 1;
	mtx_unlock(&restdata->procmtx);

	/* Set the function to be executed in the kernel for the new kthread. */
	td = FIRST_THREAD_IN_PROC(p2);
	thread_lock(td);

	/* Set the starting point of execution for the new process. */
	cpu_fork_kthread_handler(td, slsrest_metadata, (void *)args);

	/* Set the thread to be runnable, specify its priorities. */
	TD_SET_CAN_RUN(td);
	sched_prio(td, PVM);
	sched_user_prio(td, PUSER);

	/* Actually add it to the scheduler. */
	sched_add(td, SRQ_BORING);
	thread_unlock(td);

	return (0);
}

/*
 * The same as vm_object_shadow, with different refcount handling and return
 * values. We also always create a shadow, regardless of the refcount.
 */
static void
slsrest_shadow(vm_object_t shadow, vm_object_t source, vm_ooffset_t offset)
{
	/*
	 * Store the offset into the source object, and fix up the offset into
	 * the new object.
	 */
	shadow->backing_object = source;
	shadow->backing_object_offset = offset;

	VM_OBJECT_WLOCK(source);
	shadow->domain = source->domain;
	LIST_INSERT_HEAD(&source->shadow_head, shadow, shadow_list);
	source->shadow_count++;

#if VM_NRESERVLEVEL > 0
	shadow->flags |= source->flags & OBJ_COLORED;
	shadow->pg_color = (source->pg_color + OFF_TO_IDX(offset)) &
	    ((1 << (VM_NFREEORDER - 1)) - 1);
#endif
	VM_OBJECT_WUNLOCK(source);
}

static int
slsrest_dovmobjects(struct slskv_table *rectable, struct slsrest_data *restdata)
{
	struct slsvmobject slsvmobject, *slsvmobjectp;
	vm_object_t parent, object;
	struct slskv_iter iter;
	struct sls_record *rec;
	uint64_t slsid;
	size_t buflen;
	char *buf;
	int error;

	/* First pass; create of find all objects to be used. */
	KV_FOREACH(rectable, iter, slsid, rec)
	{
		buf = (char *)sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		if (rec->srec_type != SLOSREC_VMOBJ)
			continue;

		/* Get the data associated with the object in the table. */
		error = slsload_vmobject(&slsvmobject, &buf, &buflen);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		/* Restore the object. */
		error = slsrest_vmobject(&slsvmobject, restdata);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}
	}

	/* Second pass; link up the objects to their shadows. */
	KV_FOREACH(rectable, iter, slsid, rec)
	{

		if (rec->srec_type != SLOSREC_VMOBJ)
			continue;

		/*
		 * Get the object restored for the given info struct.
		 * We take advantage of the fact that the VM object info
		 * struct is the first thing in the record to typecast
		 * the latter into the former, skipping the parse function.
		 */
		slsvmobjectp = (struct slsvmobject *)sbuf_data(rec->srec_sb);
		if (slsvmobjectp->backer == 0)
			continue;

		error = slskv_find(restdata->objtable, slsvmobjectp->slsid,
		    (uintptr_t *)&object);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		/* Find a parent for the restored object, if it exists. */
		error = slskv_find(restdata->objtable,
		    (uint64_t)slsvmobjectp->backer, (uintptr_t *)&parent);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		vm_object_reference(parent);
		slsrest_shadow(object, parent, slsvmobjectp->backer_off);
	}
	SLS_DBG("Restoration of VM Objects\n");

	return (0);
}

static int
slsrest_dofiles(struct slskv_table *rectable, struct slsrest_data *restdata)
{
	struct slskv_iter iter;
	struct sls_record *rec;
	uint64_t slsid;
	int error;

	KV_FOREACH(rectable, iter, slsid, rec)
	{
		if (rec->srec_type != SLOSREC_FILE)
			continue;

		error = slsrest_dofile(
		    restdata, sbuf_data(rec->srec_sb), sbuf_len(rec->srec_sb));
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}
	}

	return (0);
}

/*
 * Shadow the objects provided by the in-memory checkpoint. That way we
 * do not destroy the in-memory checkpoint by restoring.
 */
static int
slsrest_ckptshadow(struct slsrest_data *restdata, struct slskv_table *objtable)
{
	vm_object_t obj, shadow;
	struct slskv_iter iter;
	vm_ooffset_t offset;
	int error;

	KV_FOREACH(objtable, iter, obj, shadow)
	{
		DEBUG2("Object %p has ID %lx", obj, obj->objid);
		vm_object_reference(obj);

		shadow = obj;
		offset = 0;
		vm_object_shadow(&shadow, &offset, ptoa(obj->size));

		error = slskv_add(restdata->objtable, (uint64_t)obj->objid,
		    (uintptr_t)shadow);
		if (error != 0) {
			DEBUG1("Tried to add object %lx twice", obj->objid);
			vm_object_deallocate(shadow);
			KV_ABORT(iter);
			return (error);
		}
	}

	return (0);
}

static int __attribute__((noinline))
sls_rest(struct slspart *slsp, uint64_t daemon, uint64_t rest_stopped)
{
	struct slskv_table *rectable = NULL;
	struct slskv_table *oldvntable = NULL;
	struct slsrest_data *restdata;
	struct sls_record *rec;
	struct slskv_iter iter;
	int stateerr;
	uint64_t slsid;
	size_t buflen;
	char *buf;
	int error;

	/* Get the record table from the appropriate backend. */
	SDT_PROBE0(sls, , slsrest_start, );

	/* Bring in the checkpoints from the backend. */
	restdata = uma_zalloc(slsrest_zone, M_WAITOK);
	if (restdata == NULL)
		return (ENOMEM);
	/*
	 * Shallow copy all Metropolis state. No need for reference counting,
	 * since the partition is guaranteed to be alive.
	 */
	restdata->slsmetr = slsp->slsp_metr;

	SDT_PROBE1(sls, , sls_rest, , "Creating restore data tables");

	/* Wait until we're done checkpointing to restore. */
	error = slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_RESTORING, true);
	if (error != 0) {
		/* The partition might have been detached. */
		KASSERT(slsp->slsp_status == SLSP_DETACHED,
		    ("Blocking slsp_setstate() on live partition failed"));

		uma_zfree(slsrest_zone, restdata);
		return (error);
	}

	restdata->slsp = slsp;
	SDT_PROBE1(sls, , sls_rest, , "Retrieving Partition");

	switch (slsp->slsp_target) {
	case SLS_OSD:
		/* Bring in the whole checkpoint in the form of SLOS records. */
		error = sls_read_slos(slsp, &rectable, restdata->objtable);
		if (error != 0) {
			DEBUG1("Reading the SLOS failed with %d", error);
			goto out;
		}

		/* We already have the vnodes for memory checkpoints. */
		KV_FOREACH(rectable, iter, slsid, rec)
		{
			if (rec->srec_type != SLOSREC_VNODE)
				continue;

			buf = sbuf_data(rec->srec_sb);
			buflen = sbuf_len(rec->srec_sb);

			error = slsrest_dovnode(restdata, &buf, &buflen);
			if (error != 0) {
				KV_ABORT(iter);
				goto out;
			}
		}

		SDT_PROBE1(sls, , sls_rest, , "Restoring vnodes");

		break;

	case SLS_MEM:

		/* Grab the record table of the checkpoint directly. */
		rectable = slsp->slsp_sckpt->sckpt_rectable;

		/* Replace the vnode table with that of the checkpoint. */
		oldvntable = restdata->vntable;
		restdata->vntable = slsp->slsp_sckpt->sckpt_vntable;
		error = slsrest_ckptshadow(
		    restdata, slsp->slsp_sckpt->sckpt_objtable);
		if (error != 0)
			goto out;

		SDT_PROBE1(sls, , sls_rest, , "Creating memory shadows");

		break;

	default:
		panic("Invalid target %d\n", slsp->slsp_target);
	}

	/*
	 * Recreate the VM object tree. When restoring from the SLOS we recreate
	 * everything, while when restoring from memory all anonymous objects
	 * are already there.
	 */
	error = slsrest_dovmobjects(rectable, restdata);
	if (error != 0)
		goto out;

	SDT_PROBE1(sls, , sls_rest, , "Restoring vmobjects");
	/*
	 * Iterate through the metadata; each entry represents either
	 * a process, complete with threads, FDs, and a VM map, a VM
	 * object together with its data, or a vnode/kqueue pipe or
	 * socket. Switching on the type, we use the appropriate
	 * restore routine.
	 *
	 * These entities are interdependent, but are restored independently.
	 * We mend the references they have to each other later in the code,
	 * but for now we have pointers between entities point to the slsid
	 * of the soon-to-be-restored object's record.
	 */

	/* Recreate all mbufs (to be inserted into socket buffers later). */
	KV_FOREACH(rectable, iter, slsid, rec)
	{
		if (rec->srec_type != SLOSREC_SOCKBUF)
			continue;

		buf = sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		error = slsrest_dosockbuf(buf, buflen, restdata->mbuftable);
		if (error != 0) {
			KV_ABORT(iter);
			goto out;
		}
	}

	SDT_PROBE1(sls, , sls_rest, , "Restoring sockbufs");

	error = slsrest_dofiles(rectable, restdata);
	if (error != 0)
		goto out;

	SDT_PROBE1(sls, , sls_rest, , "Restoring files");

	/* Restore all memory segments. */
	KV_FOREACH(rectable, iter, slsid, rec)
	{
		if (rec->srec_type != SLOSREC_SYSVSHM)
			continue;

		buf = sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		error = slsrest_dosysvshm(buf, buflen, restdata->objtable);
		if (error != 0) {
			KV_ABORT(iter);
			goto out;
		}
	}

	SDT_PROBE1(sls, , sls_rest, , "Restoring SYSV shared memory");

	/*
	 * Fourth pass; restore processes. These depend on the objects
	 * restored above, which we pass through the object table.
	 */
	KV_FOREACH(rectable, iter, slsid, rec)
	{
		if (rec->srec_type != SLOSREC_PROC)
			continue;

		buf = sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		error = slsrest_fork(
		    daemon, rest_stopped, buf, buflen, restdata);
		if (error != 0) {
			KV_ABORT(iter);
			goto out;
		}
	}

out:
	/* Wait until all processes are done restoring. */
	mtx_lock(&restdata->procmtx);
	while (restdata->proctds > 0)
		cv_wait(&restdata->proccv, &restdata->procmtx);

	/*
	 * We have to cleanup the fptable before we let the processes keep
	 * executing, since by doing so we mark all master terminals that
	 * weren't actually used as gone, and we need that for
	 * slsrest_ttyfixup().
	 */
	slsrest_ttyrelease(restdata);

	restdata->proctds -= 1;

	cv_broadcast(&restdata->proccv);
	mtx_unlock(&restdata->procmtx);

	SDT_PROBE1(sls, , sls_rest, , "Waiting for processes");

	/* Clean up the restore data if coming here from an error. */
	if (restdata != NULL) {
		/* Undo any fixups we did in the beginning of the function. */
		if (oldvntable != NULL)
			restdata->vntable = oldvntable;

		KASSERT(restdata->proctds == -1,
		    ("proctds is %d at free", restdata->proctds));
		uma_zfree(slsrest_zone, restdata);
	}

	stateerr = slsp_setstate(slsp, SLSP_RESTORING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("invalid state transition"));

	if (slsp->slsp_target == SLS_OSD)
		sls_free_rectable(rectable);

	SDT_PROBE1(sls, , sls_rest, , "Cleanup");
	DEBUG1("Restore daemon done with %d", error);

	return (error);
}

void
sls_restored(struct sls_restored_args *args)
{
	struct slspart *slsp = args->slsp;
	int error;

	/* Restore the old process. */
	error = sls_rest(args->slsp, args->daemon, args->rest_stopped);
	if (error != 0)
		DEBUG1("Error: sls_rest failed with %d", error);

	/* The caller is waiting for us, propagate the return value. */
	slsp_signal(slsp, error);

	/* Drop the reference we got for the SLS process. */
	slsp_deref(args->slsp);

	/* Free the arguments. */
	free(args, M_SLSMM);

	/* Release the reference to the module. */
	sls_finishop();

	kthread_exit();
}
