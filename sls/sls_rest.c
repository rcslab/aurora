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
#include <sys/sysproto.h>
#include <sys/taskqueue.h>
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

#define SLSREST_ZONEWARM (64)

SDT_PROBE_DEFINE0(sls, , , filerest_start);
SDT_PROBE_DEFINE1(sls, , , filerest_return, "int");
SDT_PROBE_DEFINE1(sls, , sls_rest, , "char *");
SDT_PROBE_DEFINE1(sls, , slsrest_metadata, , "char *");
SDT_PROBE_DEFINE0(sls, , slsrest_start, );
SDT_PROBE_DEFINE0(sls, , slsrest_end, );

static uma_zone_t slsrest_zone;

static int
slsrest_zone_ctor(void *mem, int size, void *args __unused, int flags __unused)
{
	struct slsrest_data *restdata = (struct slsrest_data *)mem;

	KASSERT(restdata->sckpt == NULL, ("sckpt in uninitialized restdata"));
	restdata->proctds = 0;

	return (0);
}

static void
slsrest_zone_dtor(void *mem, int size, void *args __unused)
{
	struct slsrest_data *restdata = (struct slsrest_data *)mem;
	struct session *sess;
	struct kqueue *kq;
	uint64_t slskn;
	slsset *kevset;
	void *slskev;
	uint64_t slsid;
	vm_object_t obj;
	struct file *fp;
	struct proc *p;
	struct pgrp *pgrp;

	if (restdata->sckpt != NULL) {
		slsckpt_drop(restdata->sckpt);
		restdata->sckpt = NULL;
	}

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
				slskq_attach(curproc, kq);
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

		free(kevset->data, M_SLSMM);
		slsset_destroy(kevset);
	}

	KV_FOREACH_POP(restdata->sesstable, slsid, sess)
	sess_release(sess);

	KV_FOREACH_POP(restdata->pgidtable, slsid, pgrp);
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
	int error;

	mtx_init(&restdata->procmtx, "SLS proc mutex", NULL, MTX_DEF);
	cv_init(&restdata->proccv, "SLS proc cv");
	restdata->proctds = -1;
	restdata->sckpt = NULL;

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

	return (0);

error:
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
	slsrest_zone = uma_zcreate("slsrest", sizeof(struct slsrest_data),
	    slsrest_zone_ctor, slsrest_zone_dtor, slsrest_zone_init,
	    slsrest_zone_fini, UMA_ALIGNOF(struct slsrest_data), 0);
	if (slsrest_zone == NULL)
		return (ENOMEM);

	uma_prealloc(slsrest_zone, SLSREST_ZONEWARM);

	return (0);
}

void
slsrest_zonefini(void)
{
	if (slsrest_zone != NULL)
		uma_zdestroy(slsrest_zone);
}

static int
slsrest_dovnode(struct slsckpt_data *sckpt, char **bufp, size_t *buflenp)
{
	struct slsvnode slsvnode;
	int error;

	error = slsload_vnode(&slsvnode, bufp, buflenp);
	if (error != 0) {
		DEBUG1("Error in slsload_vnode %d", error);
		return (error);
	}

	error = slsvn_restore_vnode(&slsvnode, sckpt);
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

	SDT_PROBE0(sls, , , filerest_start);
	error = slsrest_file(data, &slsfile, restdata);
	SDT_PROBE1(sls, , , filerest_return, slsfile.type);

	switch (slsfile.type) {
	case DTYPE_VNODE:
	case DTYPE_FIFO:
		/* Nothing to clean up. */
		break;

	case DTYPE_KQUEUE:
		/*
		 * The kqueue code creates the knote table, which is used
		 * while restoring the processes themselves and isn't cleaned
		 * here.
		 */
		break;

	case DTYPE_PIPE:
		free(((struct slspipe *)data)->data, M_SLSMM);
		free(data, M_SLSMM);
		break;

	case DTYPE_SHM:
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
	struct slsfiledesc *slsfiledesc;
	int error = 0;
	int ret;

	error = slsload_filedesc(&slsfiledesc, bufp, buflenp);
	if (error != 0)
		return (error);

	ret = slsrest_filedesc(p, slsfiledesc, restdata);
	free(slsfiledesc, M_SLSMM);

	return (ret);
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
			slskv_del(restdata->fptable, slsid);
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

	if (p->p_session == NULL || p->p_session->s_ttyp == NULL)
		return (0);

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
	uint64_t rest_stopped;
	struct slsrest_data *restdata;
};

/*
 * Restore a process' local data (threads, VM map, file descriptor table).
 */
static void
slsrest_metadata(void *args)
{
	struct slsrest_data *restdata;
	struct proc *p = curproc;
	uint64_t rest_stopped;
	size_t buflen;
	int error;
	char *buf;

	/*
	 * Transfer the arguments to the stack and free the
	 * struct we used to carry them to the new process.
	 */

	buf = ((struct slsrest_metadata_args *)args)->buf;
	buflen = ((struct slsrest_metadata_args *)args)->buflen;
	rest_stopped = ((struct slsrest_metadata_args *)args)->rest_stopped;
	restdata = ((struct slsrest_metadata_args *)args)->restdata;

	free(args, M_SLSMM);

	SLS_LOCK();
	PROC_LOCK(p);

	/* We always work on the current process. */
	thread_single(p, SINGLE_BOUNDARY);

	/* Insert the new process into Aurora. */
	sls_procadd(restdata->slsp->slsp_oid, p, false);

	SDT_PROBE1(sls, , slsrest_metadata, , "Single threading");

	PROC_UNLOCK(p);
	SLS_UNLOCK();

	error = slsvmspace_restore(p, &buf, &buflen, restdata);
	if (error != 0) {
		DEBUG1("slsvmspace_restore failed with %", error);
		goto error;
	}

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring vm state");
	/*
	 * Restore CPU state, file state, and memory
	 * state, parsing the buffer at each step.
	 */
	error = slsproc_restore(p, &buf, &buflen, restdata);
	if (error != 0) {
		DEBUG1("slsproc_restore failed with %", error);
		goto error;
	}

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring process state");
	error = slsrest_dofiledesc(p, &buf, &buflen, restdata);
	if (error != 0) {
		DEBUG1("slsrest_dofiledesc failed with %", error);
		goto error;
	}

	SDT_PROBE1(sls, , slsrest_metadata, , "Restoring file table");
	error = slskq_restore_knotes_all(p, restdata->kevtable);
	if (error != 0) {
		DEBUG1("slskq_restore_knotes_all failed with %", error);
		goto error;
	}

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
slsrest_fork(uint64_t rest_stopped, char *buf, size_t buflen,
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

static int
slsrest_dofiles(struct slsrest_data *restdata)
{
	struct slskv_iter iter;
	struct sls_record *rec;
	uint64_t slsid;
	int error;

	KV_FOREACH(restdata->sckpt->sckpt_rectable, iter, slsid, rec)
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
slsrest_ckptshadow(struct slsrest_data *restdata, struct slsckpt_data *sckpt)
{
	vm_object_t obj, shadow;
	struct slskv_iter iter;
	int error;

	/*
	 * Go through the original table apply shadowing. We do not need to
	 */
	KV_FOREACH(sckpt->sckpt_shadowtable, iter, obj, shadow)
	{
		/* XXX This should never happen afaik. Make sure and remove. */
		KASSERT(OBJT_ISANONYMOUS(obj), ("Type %d object\n", obj->type));
		DEBUG2("Object %p has ID %lx", obj, obj->objid);
		vm_object_reference(obj);

		shadow = obj;
		slsvm_object_shadowexact(&shadow);

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

/*
 * Restore an image after bringing it from the SLOS. The image is used up.
 * after restoring.
 */
static int
slsrest_make_checkpoint(struct slspart *slsp, struct slsrest_data *restdata)
{
	struct slsckpt_data *sckpt;
	struct slskv_iter iter;
	struct sls_record *rec;
	uint64_t slsid;
	size_t buflen;
	char *buf;
	int error;

	/* Possibly preload the objtable with existing values. */
	if (SLSP_DELTAREST(slsp) && slsp->slsp_sckpt != NULL) {
		error = slsrest_ckptshadow(restdata, slsp->slsp_sckpt);
		if (error != 0)
			return (error);
	}

	/* Bring in the whole checkpoint in the form of SLOS records. */
	error = sls_read_slos(slsp, &sckpt, restdata->objtable);
	if (error != 0) {
		DEBUG1("Reading the SLOS failed with %d", error);
		return (error);
	}

	/* We already have the vnodes for memory checkpoints. */
	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{
		if (rec->srec_type != SLOSREC_VNODE)
			continue;

		buf = sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		error = slsrest_dovnode(sckpt, &buf, &buflen);
		if (error != 0) {
			DEBUG2("%s: vnode restore failed with %d\n", __func__,
			    error);
			KV_ABORT(iter);
			return (error);
		}
	}

	/* Create all memory objects. */
	error = slsvmobj_restore_all(sckpt, restdata->objtable);
	if (error != 0) {
		DEBUG2("%s: restoring VM objects failed with %d\n", __func__,
		    error);
		slsckpt_drop(sckpt);
		return (error);
	}

	restdata->sckpt = sckpt;

	return (0);
}

/*
 * Create a restore image in memory. Each image is composed of a set of
 * tables holding the resources for the application. For metadata, each
 * restore deserializes metadata records found either in memory or on disk.
 * For data, restores may or may not consume the images they create. Memory
 * restores do not directly use the checkpoint data, but rather shadow the VM
 * objects.
 *
 * Disk restores may or may not consume the data they bring from disk. This is
 * a matter of policy, and we do this because it makes benchmarking more
 * straighforward. If users want to not consume the checkpoint data they bring
 * from the disk during restores, they can choose to cache the data, in which
 * case Aurora shadows the data like it does for a memory checkpoint.
 */
static int
slsrest_data(struct slspart *slsp, struct slsrest_data **restdatap)
{
	struct slsrest_data *restdata;
	int error;

	SDT_PROBE0(sls, , slsrest_start, );

	/* Bring in the checkpoints from the backend. */
	restdata = uma_zalloc(slsrest_zone, M_NOWAIT);
	if (restdata == NULL)
		return (ENOMEM);
	/*
	 * Shallow copy all Metropolis state. No need for reference counting,
	 * since the partition is guaranteed to be alive.
	 */
	restdata->slsmetr = slsp->slsp_metr;
	restdata->slsp = slsp;

	SDT_PROBE1(sls, , sls_rest, , "Initializing restdata");
	/* If we already have a checkpoint in memory, reuse it. */
	if (slsp->slsp_target != SLS_OSD) {
		if (slsp->slsp_sckpt == NULL) {
			error = EINVAL;
			goto error;
		}
		slsckpt_hold(slsp->slsp_sckpt);
		restdata->sckpt = slsp->slsp_sckpt;

		/* Create the VM object shadow table. */
		error = slsrest_ckptshadow(restdata, slsp->slsp_sckpt);
		if (error != 0) {
			DEBUG2("%s: shadowing objects failed with %d\n",
			    __func__, error);
			goto error;
		}

		SDT_PROBE1(sls, , sls_rest, , "Creating objtable");
		*restdatap = restdata;
		return (0);
	}

	/*
	 * If there is no checkpoint in memory, bring one in. This incidentally
	 * creates a new sckpt_data structure we can reuse after we are done.
	 */

	/*
	 * Read in the SLOS. This call creates the sckpt's vnode and
	 * VM object tables.
	 */
	error = slsrest_make_checkpoint(slsp, restdata);
	if (error != 0) {
		DEBUG2("%s: checkpoint creation failed with %d\n", __func__,
		    error);
		goto error;
	}

	SDT_PROBE1(sls, , sls_rest, , "Creating sckpt");
	*restdatap = restdata;

	return (0);

error:

	uma_zfree(slsrest_zone, restdata);
	return (error);
}

static int __attribute__((noinline))
sls_rest(struct slspart *slsp, uint64_t rest_stopped)
{
	struct slsrest_data *restdata;
	struct sls_record *rec;
	struct slskv_iter iter;
	int stateerr;
	uint64_t slsid;
	size_t buflen;
	char *buf;
	int error;

	/* Wait until we're done checkpointing to restore. */
	error = slsp_setstate(slsp, SLSP_AVAILABLE, SLSP_RESTORING, true);
	if (error != 0) {
		/* The partition might have been detached. */
		KASSERT(slsp->slsp_status == SLSP_DETACHED,
		    ("Blocking slsp_setstate() on live partition failed"));

		return (error);
	}

	/* Make sure an in-memory checkpoint already has data. */
	if ((slsp->slsp_attr.attr_target == SLS_MEM) &&
	    (slsp->slsp_sckpt == NULL)) {
		stateerr = slsp_setstate(
		    slsp, SLSP_RESTORING, SLSP_AVAILABLE, true);
		KASSERT(stateerr == 0, ("state error %d", stateerr));
		return (error);
	}

	/* Get the restore data from the appropriate backend. */
	error = slsrest_data(slsp, &restdata);
	if (error != 0) {
		DEBUG2("%s: restoring data failed with %d\n", __func__, error);
		stateerr = slsp_setstate(
		    slsp, SLSP_RESTORING, SLSP_AVAILABLE, true);
		KASSERT(stateerr == 0, ("state error %d", stateerr));
		return (error);
	}

	SDT_PROBE1(sls, , sls_rest, , "Caching data");

	/*
	 * Recreate the VM object tree. When restoring from the SLOS we recreate
	 * everything, while when restoring from memory all anonymous objects
	 * are already there.
	 */
	if (!slsp_rest_from_mem(slsp) && SLSP_CACHEREST(slsp)) {
		/* Cache the data if we want to. */
		slsckpt_hold(restdata->sckpt);
		slsp->slsp_sckpt = restdata->sckpt;
	}

	taskqueue_drain_all(slos.slos_tq);

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

	error = slsrest_dofiles(restdata);
	if (error != 0)
		goto out;

	SDT_PROBE1(sls, , sls_rest, , "Restoring files");

	/* Restore all memory segments. */
	KV_FOREACH(restdata->sckpt->sckpt_rectable, iter, slsid, rec)
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
	KV_FOREACH(restdata->sckpt->sckpt_rectable, iter, slsid, rec)
	{
		if (rec->srec_type != SLOSREC_PROC)
			continue;

		buf = sbuf_data(rec->srec_sb);
		buflen = sbuf_len(rec->srec_sb);

		error = slsrest_fork(rest_stopped, buf, buflen, restdata);
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

	stateerr = slsp_setstate(slsp, SLSP_RESTORING, SLSP_AVAILABLE, false);
	KASSERT(stateerr == 0, ("invalid state transition"));

	if (restdata != NULL)
		uma_zfree(slsrest_zone, restdata);

	SDT_PROBE1(sls, , sls_rest, , "Cleanup");
	DEBUG1("Restore daemon done with %d", error);

	return (error);
}

void
sls_restored(struct sls_restored_args *args)
{
	struct slspart *slsp = args->slsp;
	int error;

	if (!slsp_restorable(slsp)) {
		error = EINVAL;
		printf("WARN: Non restorable partition passed to sls_restored");
		goto out;
	}

	/* Restore the old process. */
	error = sls_rest(args->slsp, args->rest_stopped);
	if (error != 0)
		DEBUG1("Error: sls_rest failed with %d", error);

out:
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
