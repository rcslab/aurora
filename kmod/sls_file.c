#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/param.h>

#include <sys/cdefs.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/tty.h>
#include <sys/unistd.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

/* XXX Pipe has to be after selinfo */
#include <sys/pipe.h>

/* 
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

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
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_path.h"

#include "imported_sls.h"

#define DEVFS_ROOT "/dev/"
/* XXX Spin out individual operations to a new file. */

/* -------------- VNODE OPERATIONS -------------- */

/* Get the name of a vnode. This is the only information we need about it. */
static int
slsckpt_path(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	int error;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(vp, sb);
	PROC_LOCK(p);

	/* 
	 * XXX Make sure the unlinked file case is handled. 
	 */
	if (error == ENOENT)
	    return (0);

	return (0);
}

static int
slsrest_path(struct sbuf *path, struct slsfile *info, int *fdp, int ispipe)
{
	char *filepath;
	int error;
	int fd;

	filepath = sbuf_data(path);
	printf("File path %s\n", filepath);

	/* XXX Permissions/flags. Also, is O_CREAT reasonable? */
	error = kern_openat(curthread, AT_FDCWD, filepath, 
		UIO_SYSSPACE, O_RDWR, S_IRWXU);	
	if (error != 0)
	    return (error);

	fd = curthread->td_retval[0];

	/* Vnodes are seekable. Fix up the offset here. */
	if (ispipe == 0) {
	    error = kern_lseek(curthread, fd, info->offset, SEEK_SET);
	    if (error != 0)
		return (error);
	}

	/* Export the fd to the caller. */
	*fdp = fd;

	return (0);
}

/* -------------- END VNODE OPERATIONS -------------- */

static int
slsckpt_vnode(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	return slsckpt_path(p, vp, sb);
}

static int
slsrest_vnode(struct sbuf *path, struct slsfile *info, int *fdp)
{
	return slsrest_path(path, info, fdp, 0);
}

static int
slsckpt_fifo(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	return slsckpt_path(p, vp, sb);
}

static int
slsrest_fifo(struct sbuf *path, struct slsfile *info, int *fdp)
{
	return slsrest_path(path, info, fdp, 1);
}

/* -------------- START PTS OPERATIONS -------------- */

static int
slsckpt_pts_mst(struct proc *p, struct tty *pts, struct sbuf *sb)
{
	struct slspts slspts;
	int error;

	/* Get the data from the PTY. */
	slspts.magic = SLSPTS_ID;
	slspts.slsid = (uint64_t) pts;
	/* This is the master side of the pts. */
	slspts.ismaster = 1;
	/* We use the cdev as the peer's ID. */
	slspts.peerid = (uint64_t) pts->t_dev;
	slspts.drainwait = pts->t_drainwait;
	slspts.termios = pts->t_termios;
	slspts.winsize = pts->t_winsize;
	slspts.writepos = pts->t_writepos;
	slspts.termios_init_in = pts->t_termios_init_in;
	slspts.termios_init_out = pts->t_termios_init_out;
	slspts.termios_lock_in = pts->t_termios_lock_in;
	slspts.termios_lock_out = pts->t_termios_lock_out;
	slspts.flags = pts->t_flags;

	/* Add it to the record. */
	error = sbuf_bcat(sb, (void *) &slspts, sizeof(slspts));
	if (error != 0)
	    return (error);

	return (0);
}

static int
slsckpt_pts_slv(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	struct slspts slspts;
	int error;

	/* Get the data from the PTY. */
	slspts.magic = SLSPTS_ID;
	slspts.slsid = (uint64_t) vp->v_rdev;
	slspts.ismaster = 0;
	/* Our peer has the tty's pointer as its ID. */
	slspts.peerid = (uint64_t) vp->v_rdev->si_drv1;

	/* We don't need anything else, it's in the master's record. */

	/* Add it to the record. */
	error = sbuf_bcat(sb, (void *) &slspts, sizeof(slspts));
	if (error != 0)
	    return (error);

	return (0);
}

/*
 * Modified version of sys_posix_openpt(). Restores 
 * both the master and the slave side of the pts. 
 */
static int
slsrest_pts(struct slskv_table *filetable,  struct slspts *slspts, int *fdp)
{
	struct file *masterfp, *slavefp;
	int masterfd, slavefd;
	struct tty *tty;
	char *path;
	int error;

	/* 
	 * We don't really want the fd, but all the other file
	 * type restore routines create one, so we do too and
	 * get it fixed up back in the common path.
	 */
	error = falloc(curthread, &masterfp, &masterfd, O_RDWR);
	if (error != 0)
	    return (error);

	error = pts_alloc(O_RDWR|O_NOCTTY, curthread, masterfp);
	if (error != 0)
	    goto error;
    
	tty = masterfp->f_data;
	/*
	tty->t_drainwait = slspts->drainwait;
	tty->t_termios = slspts->termios;
	tty->t_winsize = slspts->winsize;
	tty->t_column = slspts->column;
	tty->t_writepos = slspts->writepos;
	tty->t_termios_init_in = slspts->termios_init_in;
	tty->t_termios_init_out = slspts->termios_init_out;
	tty->t_termios_lock_in = slspts->termios_lock_in;
	tty->t_termios_lock_out = slspts->termios_lock_out;
	*/

	KASSERT(((slspts->flags & TF_BUSY) != 0), ("PTS checkpointed while busy"));

	/* Get the name of the slave side. */
	path = malloc(PATH_MAX, M_SLSMM, M_WAITOK);
	strlcpy(path, DEVFS_ROOT, sizeof(DEVFS_ROOT));
	strlcat(path, devtoname(tty->t_dev), PATH_MAX);
	
	error = kern_openat(curthread, AT_FDCWD, path,
		UIO_SYSSPACE, O_RDWR, S_IRWXU);
	free(path, M_SLSMM);
	if (error != 0)
	    goto error;

	/* As in the case of pipes, we add the peer to the table ourselves. */
	slavefd = curthread->td_retval[0];
	slavefp = curthread->td_proc->p_fd->fd_files->fdt_ofiles[slavefd].fde_file;

	/* 
	 * We always save the peer in this function, regardless of whether it's master. 
	 * That's because the caller always looks at the slsid field, and combines it
	 * with the fd that we return to it.
	 */
	if (slspts->ismaster != 0) {
	    error = slskv_add(filetable, slspts->peerid, (uintptr_t) slavefp);
	    if (error != 0) {
		kern_close(curthread, slavefd);
		goto error;
	    }

	    *fdp = masterfd;
	    /* Get a reference on behalf of the hashtable. */
	    if (!fhold(slavefp)) {
		error = EBADF;
		goto error;
	    }
	    /* Remove it from this process and this fd. */
	    kern_close(curthread, slavefd);

	} else {
	    error = slskv_add(filetable, slspts->peerid, (uintptr_t) masterfp);
	    if (error != 0) {
		kern_close(curthread, slavefd);
		return (error);
	    }

	    *fdp = slavefd;
	    /* Get a reference on behalf of the hashtable. */
	    if (!fhold(masterfp)) {
		fdclose(curthread, masterfp, masterfd);
		return (EBADF);
	    }
	    /* Remove it from this process and this fd. */
	    kern_close(curthread, masterfd);

	}

	/* We got an extra reference, release it as in posix_openpt(). */
	fdrop(masterfp, curthread);

	return (0);

error:

	fdclose(curthread, masterfp, masterfd);
	fdrop(masterfp, curthread);

	return (error);
}
/* -------------- END PTS OPERATIONS -------------- */

static int
slsckpt_getvnode(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_vnode;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_vnode(p, fp->f_vnode, sb);
	if (error != 0)
	    return (error);

	return (0);
}

static inline int
slsckpt_getfifo(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_vnode;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	/* FIFOs are actually vnodes. */
	error = slsckpt_vnode(p, fp->f_vnode, sb);
	if (error != 0)
	    return (error);

	return (0);
}
static int
slsckpt_getkqueue(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	struct kqueue *kq;
	int error;

	/* Acquire the kqueue for reading. */
	error = kqueue_acquire(fp, &kq);
	if (error != 0)
	    return (error);

	info->backer = (uint64_t) kq;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_kqueue(p, kq, sb);
	if (error != 0)
	    goto out;

	kqueue_release(kq, 0);

	return (0);

out:
	kqueue_release(kq, 0);
	return (error);
}

static int
slsckpt_getpipe(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_data;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_pipe(p, fp, sb);
	if (error != 0)
	    return (error);

	return (0);
}

static int
slsckpt_getsocket(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->backer = (uint64_t) fp->f_data;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_socket(p, (struct socket *) fp->f_data, sb);
	if (error != 0)
	    return (error);

	return (0);

}

static uint64_t 
slsckpt_getsocket_peer(struct socket *so)
{
	struct socket *sopeer;
	struct unpcb *unpcb;

	/* Only UNIX sockets can have peers. */
	if (so->so_type != AF_UNIX)
	    return (0);

	unpcb = sotounpcb(so);

	/* Check if we have peers at all.  */
	if (unpcb->unp_conn == NULL)
	    return (0);

	/* 
	 * We have a peer if we have a bidirectional connection. 
	 * (Alternatively, it's a one-to-many datagram conn).
	 */
	if (unpcb->unp_conn->unp_conn != unpcb)
	    return (0);

	sopeer = unpcb->unp_conn->unp_socket;

	return (uint64_t) sopeer;
}

/* Get the master side of the PTS. */
static int
slsckpt_getpts_mst(struct proc *p, struct file *fp, struct tty *tty, 
	struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->type = DTYPE_PTS;
	info->backer = (uint64_t) tty; 

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_pts_mst(p, tty, sb);
	if (error != 0)
	    return (error);

	return (0);
}

/* Get the slave side of the PTS. */
static int
slsckpt_getpts_slv(struct proc *p, struct file *fp, struct vnode *vp, 
	struct slsfile *info, struct sbuf *sb)
{
	int error;

	info->type = DTYPE_PTS;
	info->backer = (uint64_t) vp->v_rdev; 

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_pts_slv(p, vp, sb);
	if (error != 0)
	    return (error);

	return (0);
}

static int
slsckpt_posixshm(struct shmfd *shmfd, struct sbuf *sb)
{
	struct slsposixshm slsposixshm;
	uint64_t len;
	int error;

	slsposixshm.slsid = (uint64_t) shmfd;
	slsposixshm.magic = SLSPOSIXSHM_ID;
	slsposixshm.mode = shmfd->shm_mode;
	slsposixshm.object = (uint64_t) shmfd->shm_object;
	slsposixshm.is_anon = (shmfd->shm_path == NULL) ? 1 : 0;

	/* 
	 * While the shmfd is unique for the shared memory, and so
	 * might be pointed to by multiple open files, the size of 
	 * the metadata is small enough that we can checkpoint 
	 * multiple times, and restore only once. 
	 */
	error = sbuf_bcat(sb, &slsposixshm, sizeof(slsposixshm));
	if (error != 0) 
	    return (error);

	/* Write down the path, if it exists. */
	if (shmfd->shm_path != NULL) {
	    len = strnlen(shmfd->shm_path, PATH_MAX);
	    error = sls_path_append(shmfd->shm_path, len, sb);
	    if (error != 0)
		return (error);
	}

	return (0);
}

static int
slsckpt_getposixshm(struct proc *p, struct file *fp, 
	struct slsfile *info, struct sbuf *sb)
{
	struct shmfd *shmfd;
	int error;

	shmfd = (struct shmfd *) fp->f_data;

	/* Finalize and write out the struct file. */
	info->type = DTYPE_SHM;
	info->backer = (uint64_t) shmfd;

	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
	    return (error);

	error = slsckpt_posixshm(shmfd, sb);
	if (error != 0)
	    return (error);

	return (0);
}

/* 
 * Get generic file info applicable to all kinds of files. 
 * While one struct file can belong to multiple descriptor
 * tables, the underlying structures (pipes, sockets, etc.)
 * belong to exactly one open file when anonymous. That means
 * that we can safely store them together.
 */
static int
slsckpt_file(struct proc *p, struct file *fp, uint64_t *slsid, struct slskv_table *objtable)
{
	vm_object_t obj, shadow;
	struct sbuf *sb, *oldsb;
	struct slsfile info;
	vm_ooffset_t offset;
	uint64_t sockid;
	int error;

	/* By default the SLS ID of the file is the file pointer. */
	*slsid = (uint64_t) fp;
	
	/* 
	 * If we have already checkpointed this open file, return success. 
	 * The caller will add the SLS ID to its file descriptor table.
	 */
	if (slskv_find(slsm.slsm_rectable, (uint64_t) fp, (uintptr_t *) &sb) == 0)
	    return (0);

	info.type = fp->f_type;
	info.flag = fp->f_flag;
	info.offset = fp->f_offset;

	info.magic = SLSFILE_ID;
	info.slsid = *slsid;

	sb = sbuf_new_auto();	

	/* Checkpoint further information depending on the file's type. */
	switch (fp->f_type) {
	/* Backed by a vnode - get the name. */
	case DTYPE_VNODE:
	    /* 
	     * In our case, vnodes are either regular, or they are ttys
	     * (the slave side, the master side is DTYPE_PTS). In the former
	     * case, we just get the name; in the latter, the name is 
	     * useless, since pts devices are interchangeable, and we
	     * do not know if the specific number will be available at
	     * restore time. We therefore use a struct slspts instead
	     * of a name to represent it.
	     */
	    if (fp->f_vnode->v_type == VREG || 
		((fp->f_vnode->v_type == VCHR) && 
		 ((fp->f_vnode->v_rdev->si_devsw->d_flags & D_TTY) == 0))) {
		/* If it's a regular file we go down the normal path. */
		error = slsckpt_getvnode(p, fp, &info, sb);
		if (error != 0)
		    goto error;

	    } else {
		/* 
		 * We use the device pointer as our ID, because it's 
		 * accessible by the master side, while our fp isn't.
		 */
		*slsid = (uint64_t)fp->f_vnode->v_rdev;
		info.slsid = *slsid;

		/* Check again if we have actually checkpointed this pts before. */
		if (slskv_find(slsm.slsm_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
		    sbuf_delete(sb);
		    return (0);
		}

		/* Otherwise we checkpoint the tty slave. */
		error = slsckpt_getpts_slv(p, fp, fp->f_vnode, &info, sb);
		if (error != 0)
		    goto error;
	    }

	    break;

	/* Backed by a fifo - only get the name */
	case DTYPE_FIFO:
	    /* Checkpoint as we would a vnode. */
	    error = slsckpt_getfifo(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;


	/* Backed by a kqueue - get all pending knotes. */
	case DTYPE_KQUEUE:
	    error = slsckpt_getkqueue(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	/* Backed by a pipe - get existing data. */
	case DTYPE_PIPE:

	    /* 
	     * In order to be able to use the open file table 
	     * to find if we need to restore a file's peer, we
	     * need to make it so that the open file and its 
	     * backing pipe have the same ID. Pass it to the caller.
	     */
	    *slsid = (uint64_t) fp->f_data;
	    info.slsid = *slsid;

	    /* 
	     * Since we're using the pipe pointer instead of the 
	     * file pointer, recheck whether we have actually already
             * checkpointed this pipe - the last test wouldn't have caught it.
	     * Having two records, one for each end of the pipe, is kinda useless
	     * right now because we don't configure the peer after restoring, but
	     * it could be used for fixing up its configuration in the future.
	     */
	    if (slskv_find(slsm.slsm_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
		sbuf_delete(sb);
		return (0);
	    }

	    error = slsckpt_getpipe(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	case DTYPE_SOCKET:
	    /* Check out if we have a paired socket. */
	    sockid = slsckpt_getsocket_peer((struct socket *) (fp->f_data));
	    if (sockid != 0) {
		/* Same problem and methodology as with pipes above. */
		*slsid = (uint64_t) fp->f_data;
		info.slsid = *slsid;

		/* Recheck - again, same as with pipes. */
		if (slskv_find(slsm.slsm_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
		    sbuf_delete(sb);
		    return (0);
		}
	    }

	    /* No peer or peer not checkpointed, go ahead. */
	    error = slsckpt_getsocket(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    break;

	case DTYPE_SHM:
	    /* Backed by POSIX shared memory, get the metadata and shadow the object. */
	    error = slsckpt_getposixshm(p, fp, &info, sb);
	    if (error != 0)
		goto error;

	    /* Lookup whether we have already shadowed the object. */
	    obj = ((struct shmfd *) fp->f_data)->shm_object;
	    error = slskv_find(objtable, (uint64_t) obj, (uintptr_t *) &shadow);
	    /* XXX Refactor into the main shadowing code. */
	    /* 
	     * If we already have a shadow, have the shared memory code point to it
	     * and transfer the reference. 
	     */
	    if (error == 0) {
		    ((struct shmfd *) fp->f_data)->shm_object = shadow;
		    vm_object_reference(shadow);
		    vm_object_deallocate(obj);

		    break;
	    }

	    /* Otherwise we have to create the shadow ourselves. */

	    /* XXX Again, we're copying ourselves here, use the existing shadowing logic. */
	    vm_object_reference(obj);

	    offset = 0;
	    shadow = obj;
	    vm_object_shadow(&shadow, &offset, ptoa(obj->size));

	    ((struct shmfd *) fp->f_data)->shm_object = shadow;
	    error = slskv_add(objtable, (uint64_t) obj, (uintptr_t) shadow);
	    KASSERT((error == 0), ("shadow already exists"));

	    break;

	case DTYPE_PTS:
	    /* 
	     * We use the pointer to the tty as our ID, because it's 
	     * accessible by the slave side, while our fp isn't.
	     */
	    *slsid = (uint64_t) fp->f_data;
	    info.slsid = *slsid;

	    /* Check again if we have actually checkpointed this pts before. */
	    if (slskv_find(slsm.slsm_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
		sbuf_delete(sb);
		return (0);
	    }

	    error = slsckpt_getpts_mst(p, fp, (struct tty *) fp->f_data, &info, sb);
	    if (error != 0)
		goto error;

	    break;
	

	default:
	    panic("invalid file type %d", fp->f_type);
	}

	/* Add the backing entities to the global tables. */
	error = slskv_add(slsm.slsm_rectable, *slsid, (uintptr_t) sb);
	if (error != 0)
	    return (error);

	error = slskv_add(slsm.slsm_typetable, (uint64_t) sb, (uintptr_t) SLOSREC_FILE);
	if (error != 0)
	    return (error);

	/* Give the SLS ID we want to use for the file pointer to the caller. */
	*slsid = info.slsid;

	return (0);

error:
	SLS_DBG("error %d", error);

	sbuf_delete(sb);

	return (error);

}

static int
slsrest_posixshm(struct slsposixshm *info, struct slskv_table *objtable, int *fdp)
{
	vm_object_t oldobj, obj;
	struct shmfd *shmfd;
	char *path;
	int error;
	int fd;

	printf("rest in\n");
	/* First and foremost, go fetch the object backing the shared memory. */
	error = slskv_find(objtable, info->object, (uintptr_t *) &obj);
	if (error != 0)
	    return (error);

	path = (info->sb != NULL) ? sbuf_data(info->sb) : SHM_ANON;

	/* First try to create the shared memory mapping. */
	error = kern_shm_open(curthread, path, UIO_SYSSPACE, 
		O_RDWR | O_CREAT | O_EXCL, info->mode, NULL);
	if (error != 0) {
	    /* Maybe it's already created then? */
	    error = kern_shm_open(curthread, path, UIO_SYSSPACE,
		    O_RDWR, info->mode, NULL);
	    if (error != 0)
		return (error);

	    /* It was - return the fd and let the main code take care of the rest. */
	    fd = curthread->td_retval[0];
	    return (0);
	}

	/* Otherwise we just created it. */
	fd = curthread->td_retval[0];

	/* Change the shared memory segment to point to the restored data. */
	shmfd = (struct shmfd *) curproc->p_fd->fd_files->fdt_ofiles[fd].fde_file->f_data;

	vm_object_reference(obj);

	oldobj = shmfd->shm_object;
	shmfd->shm_object = obj;

	vm_object_deallocate(oldobj);

	*fdp = fd;
	printf("rest out\n");

	return (0);
}

int 
slsrest_file(void *slsbacker, struct slsfile *info, struct slsrest_data *restdata)
{
	struct slspts *pts;
	struct file *fp;
	uintptr_t peer;
	uint64_t slsid;
	void *kqdata;
	int error;
	void *kq;
	int fd;

	slsid = info->slsid;

	switch(info->type) {
	case DTYPE_VNODE:
	    error = slsrest_vnode((struct sbuf *) slsbacker, info, &fd);
	    if (error != 0)
		return (error);
	    break;

	case DTYPE_FIFO:
	    error = slsrest_fifo((struct sbuf *) slsbacker, info, &fd);
	    if (error != 0)
		return (error);

	    break;

	case DTYPE_KQUEUE:
	    kqdata = ((slsset *) slsbacker)->data;
	    error = slsrest_kqueue((struct slskqueue *) kqdata, &fd);
	    if (error != 0)
		return (error);

	    /* 
	     * Associate the restored kqueue with its record. We can't 
	     * restore the kevents properly because we need to have a 
	     * fully restored file descriptor table. We therefore keep
	     * the set of kevents for the kqueue in a table until we need it.
	     */
	    kq = curproc->p_fd->fd_files->fdt_ofiles[fd].fde_file->f_data;
	    error = slskv_add(restdata->kevtable, (uint64_t) kq, (uintptr_t) slsbacker);
	    if (error != 0)
		return (error);

	    break;
	
	case DTYPE_PIPE:

	    /* 
	     * Pipes are a special case, because restoring one end 
	     * also brings back the other. For this reason, we look
	     * for the pipe's SLS ID instead of the open file's.
	     *
	     * Because the rest of the filetable's data is indexed
	     * by the ID held in the slsfile structure, we do an
	     * extra check using the ID of slspipe. If we find it,
	     * we have already restored the peer, and so we don't
	     * need to do anything.
	     */
	    slsid = ((struct slspipe *) slsbacker)->slsid;
	    if (slskv_find(restdata->filetable, slsid, &peer) == 0)
		return (0);

	    error = slsrest_pipe(restdata->filetable, (struct slspipe *) slsbacker, &fd);
	    if (error != 0)
		return (error);

	    break;

	case DTYPE_SOCKET:
	    /* Same as with pipes, check if we have already restored it. */
	    slsid = ((struct slssock *) slsbacker)->slsid;
	    if (slskv_find(restdata->filetable, slsid, &peer) == 0)
		return (0);

	    error = slsrest_socket(restdata->filetable, slsbacker, info, &fd);
	    if (error != 0)
		return (error);

	    break;

	case DTYPE_PTS:
	    /* 
	     * As with pipes, restoring one side restores the other. 
	     * Therefore, check whether we need to proceed.
	     */
	    slsid = ((struct slspts *) slsbacker)->slsid;
	    if (slskv_find(restdata->filetable, slsid, &peer) == 0)
		return (0);
	    pts = (struct slspts *) slsbacker;

	    error = slsrest_pts(restdata->filetable, (struct slspts *) slsbacker, &fd);
	    if (error != 0)
		return (error);
	    break;


	case DTYPE_SHM:
	    error = slsrest_posixshm((struct slsposixshm *) slsbacker, restdata->objtable, &fd);
	    if (error != 0)
		return (error);

	    break;

	default:
	    panic("invalid file type");
	}

	/* Get the open file from the table and add it to the hashtable. */
	fp = curthread->td_proc->p_fd->fd_files->fdt_ofiles[fd].fde_file;
	fp->f_flag = info->flag;

	error = slskv_add(restdata->filetable, info->slsid, (uintptr_t) fp);
	if (error != 0) {
	    kern_close(curthread, fd);
	    return (error);
	}

	/* We keep the open file in the filetable, so we grab a reference. */
	if (!fhold(fp)) {
	    kern_close(curthread, fd);
	    return (EBADF);
	}

	/* 
	 * Remove the open from this process' table. 
	 * When we find out we need it, we'll put
	 * it into the table of the appropriate process.
	 */
	kern_close(curthread, fd);

	return (0);
}

int
slsckpt_filedesc(struct proc *p, struct slskv_table *objtable, struct sbuf *sb)
{
	struct slsfiledesc slsfiledesc;
	struct slskv_table *fdtable;
	struct filedesc *filedesc;
	uint64_t slsid;
	struct socket *so;
	struct file *fp;
	int error = 0;
	int fd;

	error = slskv_create(&fdtable, SLSKV_NOREPLACE);
	if (error != 0)
	    return (error);

	filedesc = p->p_fd;

	vhold(filedesc->fd_cdir);
	vhold(filedesc->fd_rdir);

	slsfiledesc.fd_cmask = filedesc->fd_cmask;
	slsfiledesc.magic = SLSFILEDESC_ID;

	FILEDESC_XLOCK(filedesc);

	error = sbuf_bcat(sb, (void *) &slsfiledesc, sizeof(slsfiledesc));
	if (error != 0)
	    goto done;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(filedesc->fd_cdir, sb);
	PROC_LOCK(p);
	if (error) {
	    SLS_DBG("Error: cdir sls_vn_to_path failed with code %d\n", error);
	    goto done;
	}

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(filedesc->fd_rdir, sb);
	PROC_LOCK(p);
	if (error) {
	    SLS_DBG("Error: rdir sls_vn_to_path failed with code %d\n", error);
	    goto done;
	}

	for (fd = 0; fd <= filedesc->fd_lastfile; fd++) {
	    if (!fdisused(filedesc, fd))
		continue;

	    fp = filedesc->fd_files->fdt_ofiles[fd].fde_file;

	    /* 
	     * XXX Right now we checkpoint a subset of all file types. 
	     * Depending on what type the file is, put in some extra
	     * checks to see if we should actually checkpoint it.
	     */
	    switch (fp->f_type) {
	    case DTYPE_VNODE:
		/* 
		 * An exception to the "only handle regular files" rule.
		 * Used to handle slave PTYs, as mentioned in slsckpt_file(). 
		 */
		if ((fp->f_vnode->v_type == VCHR) && 
		    ((fp->f_vnode->v_rdev->si_devsw->d_flags & D_TTY) != 0))
		    break;

		/* 
		 * XXX Make sure /dev/{zero,null,random} are ok here
		 * unify the code path with that of the hpet0. 
		 */
		if (fp->f_vnode->v_type == VCHR)
		    break;

		/* Handle only regular vnodes for now. */
		if (fp->f_vnode->v_type != VREG)
		    continue;

		break;

	    case DTYPE_FIFO:
		/* 
		 * Fifos are allowed. They are virtually indistinguishable 
		 * form vnodes, since they are fully described by their name.
		 */
		break;

	    case DTYPE_KQUEUE:
		/* Kqueues are fine. */
		break;

	    case DTYPE_PIPE:
		/* Pipes are also fine. */
		break;
	    case DTYPE_SOCKET:
		so = (struct socket *) fp->f_data;

		/* Internet sockets are allowed. */
		if (so->so_proto->pr_domain->dom_family == AF_INET)
		    break;

		/* UNIX sockets are also allowed. */
		if (so->so_proto->pr_domain->dom_family == AF_UNIX)
		    break;

		/* 
		 * XXX Protocols we might need to support in the future:
		 * AF_INET6, AF_NETGRAPH. If we put networking-focused
		 * applications in the SLS, also AF_ARP, AF_LINK maybe?
		 */

		/* Anything else isn't. */
		continue;

	    case DTYPE_SHM:
		/* POSIX shared memory is fine too. */
		break;

	    case DTYPE_PTS:
		/* Pseudoterminals are ok. */
		break;

	    case DTYPE_DEV:
		/* 
		 * Devices _aren't_ exposed at this level, instead being
		 * backed by special vnodes in the VFS. That means that
		 * any open devices like /dev/null and /dev/zero will
		 * be of DTYPE_VNODE, and will be properly checkpointed/
		 * restored at that level (assuming they are stateless).
		 * Looking at the code, it also seems like this device 
		 * type is only used by the Linux compat layer.
		 */
		break;

	    default:
		continue;

	    }

	    /* Checkpoint the file structure itself. */
	    error = slsckpt_file(p, fp, &slsid, objtable);
	    if (error != 0)
		goto done;

	    /* Add the fd - slsid pair to the table. */
	    error = slskv_add(fdtable, fd, (uintptr_t) slsid);
	    if (error != 0)
		goto done;

	}

	/* Add the table to the descriptor table record. */
	error = slskv_serial(fdtable, sb);
	if (error != 0)
	    goto done;

done:

	vdrop(filedesc->fd_cdir);
	vdrop(filedesc->fd_rdir);

	FILEDESC_XUNLOCK(filedesc);

	slskv_destroy(fdtable);

	return (error);
}


int
slsrest_filedesc(struct proc *p, struct slsfiledesc info, 
	struct slskv_table *fdtable, struct slskv_table *filetable)
{
	struct filedesc *newfdp;
	int stdfds[] = {0, 1, 2};
	char *cdir, *rdir;
	struct file *fp;
	uint64_t slsid;
	int error = 0;
	int fd, res;

	cdir = sbuf_data(info.cdir);
	rdir = sbuf_data(info.rdir);

	/* 
	 * Create the new file descriptor table. It has 
	 * the same size as the old one, but it's empty.
	 */
	error = fdcopy_remapped(p->p_fd, stdfds, 0, &newfdp);
	if (error != 0)
	    return (error);
	fdinstall_remapped(curthread, newfdp);

	PROC_UNLOCK(p);
	error = kern_chdir(curthread, cdir, UIO_SYSSPACE);
	if (error != 0)
	    return (error);

	error = kern_chroot(curthread, rdir, UIO_SYSSPACE);
	if (error != 0)
	    return (error);
	PROC_LOCK(p);

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info.fd_cmask;
	FILEDESC_XUNLOCK(newfdp);

	/* Attach the appropriate open files to the descriptor table. */
	while (slskv_pop(fdtable, (uint64_t *) &fd, (uintptr_t *) &slsid) == 0) {

	    /* Get the restored open file from the ID. */
	    error = slskv_find(filetable, slsid, (uint64_t *) &fp);
	    if (error != 0)
		return (error);

	    /* We restore the file _exactly_ at the same fd.*/
	    error = fdalloc(curthread, fd, &res);
	    if (error != 0)
		return (error);

	    if (res != fd)
		return (error);

	    /* Get a reference to the open file for the table and install it. */
	    if (!fhold(fp)) {
		slskv_destroy(fdtable);
		return (EBADF);
	    }

	    /* 
	     * XXX Keep the UF_EXCLOSE flag with the entry somehow, 
	     * maybe using bit ops? Then again, O_CLOEXEC is most
	     * often set in the kernel by looking at the type of the
	     * file being opened. The sole exception is vnode-backed
	     * files, which seem to be able to be both.
	     */
	    _finstall(p->p_fd, fp, fd, O_CLOEXEC, NULL);
	}
	
	slskv_destroy(fdtable);

	return (0);
}
