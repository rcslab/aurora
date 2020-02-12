#include <sys/types.h>

#include <sys/capsicum.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/unistd.h>

#include <machine/param.h>
#include <machine/reg.h>
#include <machine/vmparam.h>

#include <netinet/in.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <slos.h>
#include <slos_record.h>
#include <slos_inode.h>

#include "sls_data.h"
#include "sls_file.h"
#include "sls_ioctl.h"
#include "sls_kv.h"
#include "sls_load.h"
#include "sls_mm.h"
#include "sls_partition.h"
#include "sls_proc.h"
#include "sls_sysv.h"
#include "sls_table.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"

#include "imported_sls.h"

static int
slsrest_dothread(struct proc *p, char **bufp, size_t *buflenp)
{
	struct slsthread slsthread;
	int error;

	error = slsload_thread(&slsthread, bufp, buflenp);
	if (error != 0)
	    return (error);

	error = slsrest_thread(p, &slsthread);
	if (error != 0)
	    return (error);

	return (0);
}

static int
slsrest_doproc(struct proc *p, uint64_t daemon, char **bufp, 
	size_t *buflenp, struct slsrest_data *restdata)
{
	struct slsproc slsproc; 
	struct sbuf *name;
	int error, i;

	error = slsload_proc(&slsproc, &name, bufp, buflenp);
	if (error != 0)
	    return (error);

	/* 
	 * Save the old process - new process pairs.
	 */
	error = slskv_add(restdata->proctable, (uint64_t) slsproc.slsid, (uintptr_t) p);
	if (error != 0)
	    SLS_DBG("Error: Could not add process %p to table\n", p);

	error = slsrest_proc(p, name, daemon, &slsproc, restdata);
	if (error != 0)
	    return (error);

	for (i = 0; i < slsproc.nthreads; i++) {
	    error = slsrest_dothread(p, bufp, buflenp);
	    if (error != 0)
		return (error);
	}

	return (0);
}

static int
slsrest_dofile(struct slsrest_data *restdata, char **bufp, size_t *buflenp)
{
	struct slsfile slsfile;
	struct sbuf *sb;
	void *data;
	int error;

	error = slsload_file(&slsfile, &data, bufp, buflenp);
	if (error != 0)
	    return (error);

	error = slsrest_file(data, &slsfile, restdata);
	if (error != 0)
	    return error;

	switch (slsfile.type) {
	case DTYPE_KQUEUE:
	    /* Nothing to clean up. */
	    break;

	case DTYPE_VNODE:
	    sbuf_delete((struct sbuf *) data);
	    break;

	case DTYPE_FIFO:
	    sbuf_delete((struct sbuf *) data);
	    break;

	case DTYPE_PIPE:
	case DTYPE_SOCKET:
	case DTYPE_PTS:
	    free(data, M_SLSMM);
	    break;

	case DTYPE_SHM:
	    /* XXX Refactor the sb member somehow. */
	    sb = ((struct slsposixshm *) data)->sb;
	    if (sb != NULL)
		sbuf_delete(sb);

	    free(data, M_SLSMM);
	    break;

	default:
	    return EINVAL;

	}

	return (0);
}

static int
slsrest_dofiledesc(struct proc *p, char **bufp, size_t *buflenp, 
	struct slskv_table *filetable)
{
	int error = 0;
	struct slsfiledesc slsfiledesc;
	struct slskv_table *fdtable;

	error = slsload_filedesc(&slsfiledesc, bufp, buflenp, &fdtable);
	if (error != 0)
	    return (error);

	error = slsrest_filedesc(p, slsfiledesc, fdtable, filetable);
	slskv_destroy(fdtable);
	sbuf_delete(slsfiledesc.cdir);
	sbuf_delete(slsfiledesc.rdir);
	if (error != 0) {
	    return (error);
	}

	return (error);
}

static int
slsrest_dovmspace(struct proc *p, char **bufp, size_t *buflenp, 
	struct slskv_table *objtable)
{
	struct slsvmentry slsvmentry;
	struct slsvmspace slsvmspace;
	struct shmmap_state *shmstate = NULL;
	vm_map_t map;
	int error, i;

	/* Restore the VM space and its map. */
	error = slsload_vmspace(&slsvmspace, &shmstate, bufp, buflenp);
	if (error != 0)
	    goto out;

	error = slsrest_vmspace(p, &slsvmspace, shmstate);
	if (error != 0)
	    goto out;

	map = &p->p_vmspace->vm_map;

	/* Create the individual VM entries. */
	for (i = 0; i < slsvmspace.nentries; i++) {
	    error = slsload_vmentry(&slsvmentry, bufp, buflenp);
	    if (error != 0)
		goto out;

	    error = slsrest_vmentry(map, &slsvmentry, objtable);
	    if (error != 0)
		goto out;
	}
	
out:
	free(shmstate, M_SHM);

	return (error);
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

	/* Read each segment in, restoring metadata and attaching to the object. */
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

	error = slsload_sockbuf(&m, &sbid, &buf, &bufsize);
	if (error != 0)
	    return (error);


	error = slskv_add(table, sbid, (uintptr_t) m);
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

/* Restore the kevents to the already restored kqueues. */
static int
slsrest_dokevents(struct proc *p, struct slskv_table *kevtable)
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
	    error = slskv_find(kevtable, (uint64_t) fp->f_data, (uintptr_t *) &kevset);
	    if (error != 0)
		return (error);

	    error = slsrest_kevents(fd, kevset);
	    if (error != 0)
		return (error);
	}

	return (0);
}

/* 
 * Remove /dev/pts devices without accompanying struct ttys.
 */
static int
slsrest_ttyfixup(struct proc *p)
{
	struct file *fp, *pttyfp;
	struct tty *tp;
	int error;
	int fd;

	/* 
	 * Get the terminal on top of which the restore process is running. 
	 * CAREFUL: We assume that the parent is the restore process, and it
	 * therefore has a file at fd 0 that can be used as a terminal.
	 */
	FILEDESC_XLOCK(p->p_pptr->p_fd);
	pttyfp = FDTOFP(p->p_pptr, 0);
	error = fhold(pttyfp);
	FILEDESC_XUNLOCK(p->p_pptr->p_fd);
	if (error == 0) {
	    return (EBADF);
	}

	FILEDESC_XLOCK(p->p_fd);
	for (fd = 0; fd <= p->p_fd->fd_lastfile; fd++) {
	    /* Only care about used descriptor table slots. */
	    if (!fdisused(p->p_fd, fd))
		continue;

	    /* If we're a tty, try to see if the master side is still there. */
	    fp = FDTOFP(p, fd);
	    if ((fp->f_type == DTYPE_VNODE) && 
		(fp->f_vnode->v_type == VCHR) && 
		(fp->f_vnode->v_rdev->si_devsw->d_flags & D_TTY) != 0) {

		tp = fp->f_vnode->v_rdev->si_drv1;
		if (!tty_gone(tp))
		    continue;

		/* 
		 * The master is gone, replace the file 
		 * with the parent's tty device and 
		 * adjust hold counts.
		 */
		PROC_UNLOCK(p);
		fdrop(fp, curthread);
		PROC_LOCK(p);

		if (!fhold(pttyfp))
		    goto error;

		_finstall(p->p_fd, pttyfp, fd, O_CLOEXEC, NULL);
	    }
	}
	FILEDESC_XUNLOCK(p->p_fd);

	return (0);

error:
	FILEDESC_XUNLOCK(p->p_fd);
	return (EBADF);
}

/* Struct used to set the arguments after forking. */
struct slsrest_metadata_args {
	char *buf;
	size_t buflen;
	uint64_t daemon;
	struct slsrest_data *restdata;
};

/*
 * Restore a process' local data (threads, VM map, file descriptor table).
 */
static void 
slsrest_metadata(void *args)	
{
	struct slsrest_data *restdata;
	uint64_t daemon;
	struct proc *p;
	size_t buflen;
	int error;
	char *buf;

	/* 
	 * Transfer the arguments to the stack and free the 
	 * struct we used to carry them to the new process. 
	 */

	buf = ((struct slsrest_metadata_args *) args)->buf;
	buflen = ((struct slsrest_metadata_args *) args)->buflen;
	daemon = ((struct slsrest_metadata_args *) args)->daemon;
	restdata = ((struct slsrest_metadata_args *) args)->restdata;

	free(args, M_SLSMM);

	/* We always work on the current process. */
	p = curproc;
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);
	
	/* 
	 * Restore CPU state, file state, and memory 
	 * state, parsing the buffer at each step. 
	 */
	error = slsrest_doproc(p, daemon, &buf, &buflen, restdata); 
	PROC_UNLOCK(p);
	if (error != 0)
	    goto error; 

	error = slsrest_dovmspace(p, &buf, &buflen, restdata->objtable);
	if (error != 0)
	    goto error; 

	error = slsrest_dofiledesc(p, &buf, &buflen, restdata->filetable);
	if (error != 0)
	    goto error; 

	error = slsrest_dokevents(p, restdata->kevtable);
	if (error != 0)
	    goto error; 

	/* 
	 * We're done restoring. If we were the 
	 * last restoree, notify the parent. 
	 */
	mtx_lock(&slsm.slsm_mtx);
	slsm.slsm_restoring -= 1;
	if (slsm.slsm_restoring == 0)
	    cv_signal(&slsm.slsm_proccv);


	/* Sleep until all sessions and controlling terminals are restored. */
	while (slsm.slsm_restoring >= 0) 
	    cv_wait(&slsm.slsm_donecv, &slsm.slsm_mtx);

	/* 
	 * If the original applications are running on a terminal,
	 * then the master side is not included in the checkpoint. Since
	 * in that case it doesn't make much sense restoring the terminal,
	 * we instead pass the original restore process' terminal as an
	 * argument.
	 */
	mtx_unlock(&slsm.slsm_mtx);

	PROC_LOCK(p);
	error = slsrest_ttyfixup(p);
	if (error != 0)
	    SLS_DBG("tty_fixup failed with %d\n", error);
	kern_psignal(p, SIGCONT);

	PROC_UNLOCK(p);

	kthread_exit();


	panic("Having the kthread exit failed");

	PROC_UNLOCK(p);

	return;

error:

	printf("Error %d while restoring process\n", error);
	mtx_lock(&slsm.slsm_mtx);
	slsm.slsm_restoring -= 1;
	if (slsm.slsm_restoring == 0)
	    cv_signal(&slsm.slsm_proccv);
	mtx_unlock(&slsm.slsm_mtx);

	exit1(curthread, error, 0);
}

static int
slsrest_fork(uint64_t daemon, char *buf, size_t buflen, 
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
	fr.fr_flags = RFFDG | RFPROC | RFSTOPPED;
	fr.fr_procp = &p2;

	error = fork1(curthread, &fr);
	if (error != 0)
	    return (error);


	args = malloc(sizeof(*args), M_SLSMM, M_WAITOK);
	args->buf = buf;
	args->daemon = daemon;
	args->buflen = buflen;
	args->restdata = restdata;

	/* 
	 * Set the function to be executed in the kernel for the 
	 * process' new kthread.
	 */
	td = FIRST_THREAD_IN_PROC(p2);
	thread_lock(td);

	cpu_fork_kthread_handler(td, slsrest_metadata, (void *) args);

	/* Set the thread to be runnable, specify its priorities. */
	TD_SET_CAN_RUN(td);
	sched_prio(td, PVM);
	sched_user_prio(td, PUSER);

	/* Actually add it to the scheduler. */
	sched_add(td, SRQ_BORING);
	thread_unlock(td);

	/* Note down the fact that one more process is being restored. */
	mtx_lock(&slsm.slsm_mtx);
	slsm.slsm_restoring += 1;
	mtx_unlock(&slsm.slsm_mtx);

	return (0);
}

/*
 * The same as vm_object_shadow, with different refcount handling and return values.
 * We also always create a shadow, regardless of the refcount.
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

	/* 
	* If this is the first shadow, then we transfer the reference
	* from the caller to the shadow, as done in vm_object_shadow.
	* Otherwise we add a reference to the shadow.
	*/
	if (source->shadow_count != 0)
	    source->ref_count += 1;

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
slsrest_dovmobjects(struct slskv_table *metatable, struct slskv_table *datatable, 
	struct slskv_table *objtable)
{
	struct slsvmobject slsvmobject, *slsvmobjectp;
	vm_object_t parent, object;
	struct slsdata *slsdata;
	struct slskv_iter iter;
	struct slos_rstat *st;
	size_t buflen;
	void *record;
	char *buf;
	int error;

	/* First pass; create of find all objects to be used. */
	KV_FOREACH(metatable, iter, record, st) {
	    buf = (char *) record;
	    buflen = st->len;

	    if (st->type != SLOSREC_VMOBJ)
		continue;

	    /* Get the data associated with the object in the table. */
	    error =  slsload_vmobject(&slsvmobject, &buf, &buflen);
	    if (error != 0) {
		KV_ABORT(iter);
		return (error);
	    }

	    /* Find the data associated with the object. */

	    slsdata = NULL;
	    if (datatable != NULL) {
		error = slskv_find(datatable, (uint64_t) record, (uintptr_t *) &slsdata);
		if (error != 0) {
		    KV_ABORT(iter);
		    return (error);
		}
	    }

	    /* Restore the object. */
	    error = slsrest_vmobject(&slsvmobject, objtable, slsdata);
	    if (error != 0) {
		KV_ABORT(iter);
		return (error);
	    }

	}

	/* Second pass; link up the objects to their shadows. */
	KV_FOREACH(metatable, iter, record, st) {

	    if (st->type != SLOSREC_VMOBJ)
		continue;
	    /* 
	     * Get the object restored for the given info struct. 
	     * We take advantage of the fact that the VM object info
	     * struct is the first thing in the record to typecast
	     * the latter into the former, skipping the parse function.
	     */
	    slsvmobjectp = (struct slsvmobject *) record; 
	    error = slskv_find(objtable, slsvmobjectp->slsid, (uintptr_t *) &object);
	    if (error != 0) {
		KV_ABORT(iter);
		return (error);
	    }

	    /* Try to find a parent for the restored object, if it exists. */
	    error = slskv_find(objtable, (uint64_t) slsvmobjectp->backer, (uintptr_t *) &parent);
	    if (error != 0) {
		continue;
	    }
	    slsrest_shadow(object, parent, slsvmobjectp->backer_off);
	}
	SLS_DBG("Restoration of VM Objects\n");

	return (0);
}

static int
slsrest_dofiles(struct slskv_table *metatable, struct slsrest_data *restdata)
{
	struct slskv_iter iter;
	struct slos_rstat *st;
	void *record;
	int error;

	KV_FOREACH(metatable, iter, record, st) {
	    if (st->type != SLOSREC_FILE)
		continue;

	    error = slsrest_dofile(restdata, (char **) &record, &st->len);
	    if (error != 0) {
		KV_ABORT(iter);
		return (error);
	    }
	}

	return (0);
}

static void 
slsrest_fini(struct slsrest_data *restdata)
{
	uint64_t kq;
	slsset *kevset;
	void *slskev; 
	uint64_t slsid;
	struct file *fp;
	struct mbuf *m, *headm;

	cv_destroy(&restdata->proccv);
	mtx_destroy(&restdata->procmtx);

	if (restdata->mbuftable != NULL) {
	    KV_FOREACH_POP(restdata->mbuftable, slsid, headm) {
		while (headm != NULL) {
		    m = headm;
		    headm = headm->m_nextpkt;
		    m_free(m);
		}
	    }

	    slskv_destroy(restdata->mbuftable);
	}

	if (restdata->sesstable != NULL)
	    slskv_destroy(restdata->sesstable);

	if (restdata->pgidtable != NULL)
	    slskv_destroy(restdata->pgidtable);

	if (restdata->objtable != NULL)
	    slskv_destroy(restdata->objtable);

	if (restdata->proctable != NULL)
	    slskv_destroy(restdata->proctable);

	if (restdata->filetable != NULL) {
	    KV_FOREACH_POP(restdata->filetable, slsid, fp)
		fdrop(fp, curthread);

	    slskv_destroy(restdata->filetable);
	}

	/* Each value in the table is a set of kevents. */
	if (restdata->kevtable != NULL) {
	    KV_FOREACH_POP(restdata->kevtable, kq, kevset) {
		KVSET_FOREACH_POP(kevset, slskev)
		    free(slskev, M_SLSMM);

		slsset_destroy(kevset);
	    }

	    slskv_destroy(restdata->kevtable);
	}

	free(restdata, M_SLSMM);
}

static int
slsrest_init(struct slsrest_data **restdatap)
{
	struct slsrest_data *restdata;
	int error;

	restdata = malloc(sizeof(*restdata), M_SLSMM, M_WAITOK | M_ZERO);

	/* Initialize the necessary tables. */
	error = slskv_create(&restdata->objtable);
	if (error != 0)
	    goto error;

	error = slskv_create(&restdata->proctable);
	if (error != 0)
	    goto error;

	error = slskv_create(&restdata->filetable);
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
	
	mtx_init(&restdata->procmtx, "SLS proc mutex", NULL, MTX_DEF);
	cv_init(&restdata->proccv, "SLS proc cv");
	
	*restdatap = restdata;
	return (0);
	
error:
	slsrest_fini(restdata);
	return (error);
}

static int
sls_rest(struct proc *p, uint64_t oid, uint64_t daemon)
{
	struct slskv_table *metatable = NULL, *datatable = NULL;
	struct slspagerun *pagerun, *tmppagerun;
	struct slsrest_data *restdata;
	struct slsdata *slsdata;
	vm_object_t obj, shadow;
	struct sls_record *rec;
	struct slskv_iter iter;
	struct slos_rstat *st;
	struct slos_node *vp;
	struct slspart *slsp;
	struct sbuf *record;
	uint64_t slsid;
	size_t buflen;
	char *buf;
	int error;

	/* Get the record table from the appropriate backend. */

	/* Bring in the checkpoints from the backend. */
	error = slsrest_init(&restdata);
	if (error != 0)
	    return (error);

	/* Check if the requested partition exists. */
	slsp = slsp_find(oid);
	if (slsp == NULL)
	    goto out;
	
	switch (slsp->slsp_attr.attr_target) {
	case SLS_OSD:

	    /* XXX Temporary until we change to multiple inodes per checkpoint. */
	    SLS_DBG("Opening inode\n");
	    vp = slos_iopen(&slos, oid);
	    if (vp == NULL)
		return (EIO); 

	    SLS_DBG("Reading in data\n");

	    /* Bring in the whole checkpoint in the form of SLOS records. */
	    error = sls_read_slos(vp, &metatable, &datatable);
	    if (error != 0) {
		slos_iclose(&slos, vp);
		goto out;
	    }

	    SLS_DBG("Closing inode\n");
	    error = slos_iclose(&slos, vp); 
	    if (error != 0) {
		SLS_DBG("Error closing\n");
		goto out;
	    }
	    break;

	case SLS_MEM:
	    error = slskv_create(&metatable);
	    if (error != 0)
		goto out;

	    KV_FOREACH(slsp->slsp_sckpt->sckpt_rectable, iter, slsid, rec) {

		st = malloc(sizeof(*st), M_SLSMM, M_WAITOK);
		st->type = rec->srec_type;
		st->len = sbuf_len(rec->srec_sb);

		error = slskv_add(metatable, (uint64_t) sbuf_data(rec->srec_sb), (uintptr_t) st);
		if (error != 0) {
		    free(st, M_SLSMM);
		    KV_ABORT(iter);
		    goto out;
		}
	    }

	    KV_FOREACH(slsp->slsp_sckpt->sckpt_objtable, iter, obj, shadow) {
		vm_object_reference(obj);

		vm_object_t shadow = obj;
		vm_ooffset_t offset = 0;
		vm_object_shadow(&shadow, &offset, ptoa(obj->size));

		error = slskv_add(restdata->objtable, (uint64_t) obj, (uintptr_t) shadow);
		if (error != 0) {
		    vm_object_deallocate(shadow);
		    KV_ABORT(iter);
		    goto out;
		}
	    }

	    break;

	default:
	    panic("Invalid target %d\n", slsp->slsp_attr.attr_target);
	}

	/* 
	 * Recreate the VM object tree. When restoring from the SLOS we recreate everything, while
	 * when restoring from memory all anonymous objects are already there.
	 */
	error = slsrest_dovmobjects(metatable, datatable, restdata->objtable);
	if (error != 0)
	    goto out;


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
	KV_FOREACH(metatable, iter, record, st) {
	    if (st->type != SLOSREC_SOCKBUF)
		continue;

	    buf = (char *) record;
	    buflen = st->len;

	    error = slsrest_dosockbuf(buf, buflen, restdata->mbuftable);
	    if (error != 0) {
		KV_ABORT(iter);
		goto out;
	    }
	}

	error = slsrest_dofiles(metatable, restdata);
	if (error != 0)
	    goto out;

	/* Restore all memory segments. */
	KV_FOREACH(metatable, iter, record, st) {
	    if (st->type != SLOSREC_SYSVSHM)
		continue;

	    buf = (char *) record;
	    buflen = st->len;

	    error = slsrest_dosysvshm(buf, buflen, restdata->objtable);
	    if (error != 0) {
		KV_ABORT(iter);
		goto out;
	    }
	}


	/* 
	 * Fourth pass; restore processes. These depend on the objects 
	 * restored above, which we pass through the object table.
	 */
	KV_FOREACH(metatable, iter, record, st) {
	    if (st->type != SLOSREC_PROC)
		continue;

	    buf = (char *) record;
	    buflen = st->len;

	    error = slsrest_fork(daemon, buf, buflen, restdata);
	    if (error != 0) {
		KV_ABORT(iter);
		goto out;
	    }
	}

	/* Wait until all processes are done restoring. */
	mtx_lock(&slsm.slsm_mtx);
	while (slsm.slsm_restoring > 0)
	    cv_wait(&slsm.slsm_proccv, &slsm.slsm_mtx);

	/* We push the counter below 0 and restore all sessions. */
	slsm.slsm_restoring -= 1;

	/* 
	 * The tables in restdata only hold key-value pairs, cleanup is easy. 
	 * Note that we have to cleanup the filetable before we let the
	 * processes keep executing, since by doing so we mark all master
	 * terminals that weren't actually used as gone, and we need 
	 * that for slsrest_ttyfixup().
	 */
	slsrest_fini(restdata);
	restdata = NULL;

	/* Restore all sessions. */
	cv_broadcast(&slsm.slsm_donecv);

	mtx_unlock(&slsm.slsm_mtx);

	SLS_DBG("Done\n");

out:
	/* Cleanup the tables used for bringing the data in memory. */
	if ((metatable != NULL) && (slsp->slsp_attr.attr_target != SLS_MEM)) {
	    KV_FOREACH_POP(metatable, record, st) {
		free(st, M_SLSMM);
		free(record, M_SLSMM);
	    }

	    slskv_destroy(metatable);
	}
	
	if ((datatable != NULL) && (slsp->slsp_attr.attr_target != SLS_MEM)) {
	    KV_FOREACH_POP(datatable, record, slsdata) {
		LIST_FOREACH_SAFE(pagerun, slsdata, next, tmppagerun) {
		    LIST_REMOVE(pagerun, next);
		    free(pagerun->data, M_SLSMM);
		    uma_zfree(slspagerun_zone, pagerun);
		}
		free(slsdata, M_SLSMM);
	    }
	    /* The table itself. */
	    slskv_destroy(datatable);
	}

	/* Leave the reference we got when searching for the partition. */
	if (slsp != NULL)
	    slsp_deref(slsp);

	/* Clean up the restore data if coming here from an error. */
	if (restdata != NULL)
	   slsrest_fini(restdata);

	return (error);
}

void
sls_restored(struct sls_restored_args *args)
{
	int error;

	slsm.slsm_restoring = 0;
	/* Restore the old process. */
	error = sls_rest(curproc, args->oid, args->daemon);
	if (error != 0)
	    printf("Error: sls_rest failed with %d\n", error);

	/* Free the arguments */
	free(args, M_SLSMM);

	return;
}
