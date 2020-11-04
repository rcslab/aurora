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
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "sls_path.h"
#include "sls_vm.h"

#include "debug.h"

SDT_PROBE_DEFINE1(sls, , , fileckptstart, "int");
SDT_PROBE_DEFINE1(sls, , , fileckptend, "int");
SDT_PROBE_DEFINE1(sls, , , fileckpterr, "int");

static int
slsckpt_getvnode(struct proc *p, struct file *fp, struct slsfile *info,
    struct sbuf *sb, int ign_unlink)
{
	int error;

	info->backer = (uint64_t) fp->f_vnode;

	/* Get the location of the node in the VFS/SLSFS. */
	error = slsckpt_vnode(p, fp->f_vnode, info, sb, ign_unlink);
	if (error != 0)
		return (error);

	return (0);
}

static inline int
slsckpt_getfifo(struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb,
    int ign_unlink)
{
	return slsckpt_getvnode(p, fp, info, sb, ign_unlink);
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
slsckpt_getsocket(struct proc *p, struct file *fp, struct slsfile *info, 
    struct sbuf *sb, struct slsckpt_data *sckpt_data)
{
	int error;

	info->backer = (uint64_t) fp->f_data;

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) info, sizeof(*info));
	if (error != 0)
		return (error);

	error = slsckpt_socket(p, (struct socket *) fp->f_data, sb, sckpt_data);
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
slsckpt_file(struct proc *p, struct file *fp, uint64_t *slsid, struct slsckpt_data *sckpt_data)
{
	vm_object_t obj, shadow;
	struct sbuf *sb, *oldsb;
	struct sls_record *rec;
	struct slsfile info;
	uint64_t sockid;
	int error;

	/* By default the SLS ID of the file is the file pointer. */
	*slsid = (uint64_t) fp;

	/* 
	 * If we have already checkpointed this open file, return success. 
	 * The caller will add the SLS ID to its file descriptor table.
	 */
	if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) fp, (uintptr_t *) &sb) == 0)
		return (0);

	SDT_PROBE1(sls, , , fileckptstart, fp->f_type);

	info.type = fp->f_type;
	info.flag = fp->f_flag;
	info.offset = fp->f_offset;
	info.has_path = 0;

	/* 
	 * XXX Change SLS ID to the encoding used for the rest of the objects.  
	 */
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
			error = slsckpt_getvnode(p, fp, &info, sb,
			    sckpt_data->sckpt_attr.attr_flags & SLSATTR_IGNUNLINKED);
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
			if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
				sbuf_delete(sb);
				SDT_PROBE1(sls, , , fileckptend, fp->f_type);
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
		error = slsckpt_getfifo(p, fp, &info, sb,
		    sckpt_data->sckpt_attr.attr_flags & SLSATTR_IGNUNLINKED);
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
		if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
			sbuf_delete(sb);
			SDT_PROBE1(sls, , , fileckptend, fp->f_type);
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
			if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
				sbuf_delete(sb);
				SDT_PROBE1(sls, , , fileckptend, fp->f_type);
				return (0);
			}
		}

		/* No peer or peer not checkpointed, go ahead. */
		error = slsckpt_getsocket(p, fp, &info, sb, sckpt_data);
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
		error = slskv_find(sckpt_data->sckpt_objtable, (uint64_t) obj, (uintptr_t *) &shadow);

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

		error = slsvm_object_shadow(sckpt_data->sckpt_objtable, &obj);
		KASSERT(error == 0, ("object %p already has a shadow", obj));
		((struct shmfd *) fp->f_data)->shm_object = obj;

		break;

	case DTYPE_PTS:
		/* 
		 * We use the pointer to the tty as our ID, because it's 
		 * accessible by the slave side, while our fp isn't.
		 */
		*slsid = (uint64_t) fp->f_data;
		info.slsid = *slsid;

		/* Check again if we have actually checkpointed this pts before. */
		if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) *slsid, (uintptr_t *) &oldsb) == 0) {
			sbuf_delete(sb);
			SDT_PROBE1(sls, , , fileckptend, fp->f_type);
			return (0);
		}

		error = slsckpt_getpts_mst(p, fp, (struct tty *) fp->f_data, &info, sb);
		if (error != 0)
			goto error;

		break;


	default:
		panic("invalid file type %d", fp->f_type);
	}

	sbuf_finish(sb);

	rec = sls_getrecord(sb, *slsid, SLOSREC_FILE);
	/* Add the backing entities to the global tables. */
	error = slskv_add(sckpt_data->sckpt_rectable, *slsid, (uintptr_t) rec);
	if (error != 0) {
		free(rec, M_SLSMM);
		goto error;
	}


	/* Give the SLS ID we want to use for the file pointer to the caller. */
	*slsid = info.slsid;

	SDT_PROBE1(sls, , , fileckptend, fp->f_type);
	return (0);

error:
	SLS_DBG("error %d", error);

	sbuf_delete(sb);
	SDT_PROBE1(sls, , , fileckpterr, fp->f_type);

	return (error);

}

int 
slsrest_file(void *slsbacker, struct slsfile *info, struct slsrest_data *restdata)
{
	struct slspipe *slspipe;
	struct pipe *pipepeer;
	struct kqueue *kq;
	struct vnode *vp;
	struct file *fp, *fppeer;
	uintptr_t peer;
	uint64_t slsid;
	void *kqdata;
	int error;
	int fd;

	switch(info->type) {
	case DTYPE_VNODE:
		error = slsrest_vnode((struct sbuf *) slsbacker, info, &fd, 1);
		if (error != 0)
			return (error);

		fp = FDTOFP(curproc, fd);
		vp = fp->f_vnode;
		vref(vp);

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
		fp = FDTOFP(curproc, fd);
		kq = fp->f_data;
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
		 * we have already restored the peer, and so we only need
		 * to restore the data for the backer.
		 */
		slspipe = (struct slspipe *) slsbacker;
		slsid = slspipe->slsid;
		if (slskv_find(restdata->filetable, slsid, (uintptr_t *) &fppeer) == 0) {
			pipepeer = (struct pipe *) fppeer->f_data;

			/* Restore the buffer's state. */
			pipepeer->pipe_buffer.cnt = slspipe->pipebuf.cnt;
			pipepeer->pipe_buffer.in = slspipe->pipebuf.in;
			pipepeer->pipe_buffer.out = slspipe->pipebuf.out;
			/* Check if the data fits in the newly created pipe. */
			if (pipepeer->pipe_buffer.size < slspipe->pipebuf.cnt)
				return (EINVAL);

			memcpy(pipepeer->pipe_buffer.buffer, slspipe->data, slspipe->pipebuf.cnt);

			return (0);
		}

		error = slsrest_pipe(restdata->filetable, (struct slspipe *) slsbacker, &fd);
		if (error != 0)
			return (error);

		break;

	case DTYPE_SOCKET:
		/* Same as with pipes, check if we have already restored it. */
		slsid = ((struct slssock *) slsbacker)->slsid;
		if (slskv_find(restdata->filetable, slsid, &peer) == 0)
			return (0);

		error = slsrest_socket(restdata->filetable, restdata->mbuftable, slsbacker, info, &fd);
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
	fp = FDTOFP(curproc, fd);
	fp->f_flag = info->flag;
	fp->f_offset= info->offset;

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

/* List of paths for device vnodes that can be included in a checkpoint. */
char *sls_accepted_devices[] = {
	"/dev/null",
	"/dev/zero",
	"/dev/hpet0",
	"/dev/random",
	"/dev/urandom",
	NULL,
};

static int
slsckpt_is_accepted_device(struct vnode *vp)
{
	struct sbuf *sb;
	char *path;
	int error;
	int i;

	/* Get the path of the vnode. */
	error = sls_vn_to_path(vp, &sb);
	if (error != 0) {
		SLS_DBG("Could not get path for character device vnode %p", vp);
		return (0);
	}

	path = sbuf_data(sb);

	for (i = 0; sls_accepted_devices[i] != NULL; i++) {
		if (strncmp(path, sls_accepted_devices[i], PATH_MAX) != 0) {
			sbuf_delete(sb);
			return (1);
		}
	}
	sbuf_delete(sb);

	return (0);
}

static int
slsckpt_file_supported(struct file *fp)
{
	struct socket *so;

	switch (fp->f_type) {
	case DTYPE_VNODE:
		/* 
		 * An exception to the "only handle regular files" rule.
		 * Used to handle slave PTYs, as mentioned in slsckpt_file(). 
		 */
		if ((fp->f_vnode->v_type == VCHR) && 
		    ((fp->f_vnode->v_rdev->si_devsw->d_flags & D_TTY) != 0))
			return (1);

		if (fp->f_vnode->v_type == VCHR)
			return (slsckpt_is_accepted_device(fp->f_vnode));

		/* Handle only regular vnodes for now. */
		if (fp->f_vnode->v_type != VREG)
			return (0);

		return (1);

	case DTYPE_SOCKET:
		so = (struct socket *) fp->f_data;

		/* IPv4 and UNIX sockets are allowed. */
		if ((so->so_proto->pr_domain->dom_family == AF_INET) ||
		    (so->so_proto->pr_domain->dom_family == AF_UNIX))
			return (1);

		/* Anything else isn't. */
		return (0);

		/* 
		 * XXX Protocols we might need to support in the future:
		 * AF_INET6, AF_NETGRAPH. If we put networking-focused
		 * applications in the SLS, also AF_ARP, AF_LINK maybe?
		 */

	case DTYPE_DEV:
		/* Devices are accessed as VFS nodes. */
		return (0);

		/* 
		 * Devices aren't exposed at this level, instead being
		 * backed by special vnodes in the VFS. That means that
		 * any open devices like /dev/null and /dev/zero will
		 * be of DTYPE_VNODE, and will be properly checkpointed/
		 * restored at that level (assuming they are stateless).
		 * Looking at the code, it also seems like this device 
		 * type is only used by the Linux compat layer.
		 */

	case DTYPE_FIFO:
	case DTYPE_KQUEUE:
	case DTYPE_PIPE:
	case DTYPE_SHM:
	case DTYPE_PTS:
		/* These types are always ok. */
		return (1);

	default:
		/* Otherwise we ignore the file. */
		return (0);

	}
}

int
slsckpt_filedesc(struct proc *p, struct slsckpt_data *sckpt_data, struct sbuf *sb)
{
	struct slsfiledesc slsfiledesc;
	struct slskv_table *fdtable;
	struct filedesc *filedesc;
	uint64_t slsid;
	struct file *fp;
	int error = 0;
	int fd;

	/* 
	 * The table of file descriptors to be created. We use this instead
	 * of an array for the file descriptor table.
	 */
	error = slskv_create(&fdtable);
	if (error != 0)
		return (error);

	filedesc = p->p_fd;

	vhold(p->p_fd->fd_cdir);
	vhold(p->p_fd->fd_rdir);

	slsfiledesc.fd_cmask = filedesc->fd_cmask;
	slsfiledesc.magic = SLSFILEDESC_ID;

	FILEDESC_XLOCK(filedesc);

	error = sbuf_bcat(sb, (void *) &slsfiledesc, sizeof(slsfiledesc));
	if (error != 0)
		goto done;

	/* Get the current and root directories. XXX Get the jail directory if it exists. */
	error = sls_vn_to_path_append(filedesc->fd_cdir, sb);
	if (error) {
		SLS_DBG("Error: cdir sls_vn_to_path failed with code %d\n", error);
		goto done;
	}

	error = sls_vn_to_path_append(filedesc->fd_rdir, sb);
	if (error) {
		SLS_DBG("Error: rdir sls_vn_to_path failed with code %d\n", error);
		goto done;
	}

	/* Scan the entries of the table for active file descriptors. */
	for (fd = 0; fd <= filedesc->fd_lastfile; fd++) {
		if (!fdisused(filedesc, fd))
			continue;

		DEBUG1("Checkpointing fd %d", fd);
		KASSERT((filedesc == p->p_fd), ("wrong file descriptor table"));
		fp = FDTOFP(p, fd);

		/* Check if the file is currently supported by SLS. */
		if (!slsckpt_file_supported(fp))
			continue;

		/* Checkpoint the file structure itself. */
		error = slsckpt_file(p, fp, &slsid, sckpt_data);
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
	uint64_t fd;
	int res;

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

	error = kern_chdir(curthread, cdir, UIO_SYSSPACE);
	if (error != 0)
		return (error);

	error = kern_chroot(curthread, rdir, UIO_SYSSPACE);
	if (error != 0)
		return (error);

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info.fd_cmask;

	/* Attach the appropriate open files to the descriptor table. */
	KV_FOREACH_POP(fdtable, fd, slsid) {

		/* Get the restored open file from the ID. */
		error = slskv_find(filetable, slsid, (uint64_t *) &fp);
		if (error != 0)
			goto error;

		/* We restore the file _exactly_ at the same fd.*/
		error = fdalloc(curthread, fd, &res);
		if (error != 0)
			goto error;

		if (res != fd) {
			error = EINVAL;
			goto error;
		}

		/* Get a reference to the open file for the table and install it. */
		if (!fhold(fp)) {
			error = EBADF;
			goto error;
		}


		/* 
		 * XXX Keep the UF_EXCLOSE flag with the entry somehow, 
		 * maybe using bit ops? Then again, O_CLOEXEC is most
		 * often set in the kernel by looking at the type of the
		 * file being opened. The sole exception is vnode-backed
		 * files, which seem to be able to be both.
		 */
		_finstall(p->p_fd, fp, fd, O_CLOEXEC, NULL);

		/* Fix up the kqueue to point to its new filetable. */
		if (fp->f_type == DTYPE_KQUEUE)
			slsrest_kqattach_locked(p, (struct kqueue *) fp->f_data);
	}

	FILEDESC_XUNLOCK(newfdp);

	return (0);

error:

	FILEDESC_XUNLOCK(newfdp);

	return (error);
}
