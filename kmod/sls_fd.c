#include <sys/param.h>

#include <machine/param.h>

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
#include <sys/unistd.h>
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

#include "slsmm.h"
#include "sls_path.h"
#include "sls_data.h"
#include "sls_fd.h"
#include "sls.h"

#include "imported_sls.h"

struct kevent_info sls_kevents[1024]; 

/* Checkpoint a kqueue and all of its pending knotes. */
static int
sls_kqueue_ckpt(struct proc *p, struct file *fp, struct sbuf *sb)
{
	struct kqueue *kq;
	struct kqueue_info kqinfo;
	struct knote *kn;
	uint64_t numevents = 0;
	int error, i;

	/* Acquire the kqueue for reading. */
	error = kqueue_acquire(fp, &kq);
	if (error != 0)
	    return error;

	/* Traverse all lists of knotes in the kqueue. */
	for (i = 0; i < kq->kq_knlistsize; i++) {
	    /* Get all knotes from each list. */
	    SLIST_FOREACH(kn, &kq->kq_knlist[i], kn_link) {
		/* 
		 * XXX Check whether any knotes are in flux. 
		 * This is erring on the side of caution, since
		 * I'm not sure what being in flux would do for us.
		 */
		if (kn != NULL && (kn->kn_influx > 0))
		    panic("knote in flux while checkpointing");
		
		/* 
		 * 
		 * Get all relevant fields. Most of them are 
		 * from the kevent struct the knote points to.
		 */
		/*
		 * XXX Right now we're using a static buffer
		 * that we assume will suffice for 1 kqueue.
		 * What we truly need is a queue that we can
		 * write events to. Until then, this suffices.
		 */
		sls_kevents[numevents].status = kn->kn_status;
		sls_kevents[numevents].ident = kn->kn_kevent.ident;
		sls_kevents[numevents].filter = kn->kn_kevent.filter;
		sls_kevents[numevents].flags = kn->kn_kevent.flags;
		sls_kevents[numevents].fflags = kn->kn_kevent.fflags;
		sls_kevents[numevents].data = kn->kn_kevent.data;
		sls_kevents[numevents].magic = SLS_KQUEUE_INFO_MAGIC;
		numevents += 1;
	    }
	}

	/* Create the structure for the kqueue itself. */
	kqinfo.magic = SLS_KQUEUE_INFO_MAGIC;
	kqinfo.numevents = numevents;

	/* Write the kqueue itself to the sbuf. */
	error = sbuf_bcat(sb, (void *) &kqinfo, sizeof(kqinfo));
	if (error != 0)
	    goto out;

	/* Write each kevent to the sbuf. */
	error = sbuf_bcat(sb, (void *) sls_kevents, numevents * sizeof(sls_kevents[0]));
	if (error != 0)
	    goto out;

out:
	/* Release the reference to the kqueue. */
	kqueue_release(kq, 0);

	return 0;
}

/* Get the name of a vnode. This is the only information we need about it. */
static int
sls_vnode_ckpt(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	int error;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(vp, sb);
	PROC_LOCK(p);

	/* 
	 * XXX Handle later, right now the vnode might 
	 * be unlinked, and therefore have no name.
	 */
	if (error == ENOENT)
	    return 0;

	return 0;
}

static int
sls_pipe_ckpt(struct proc *p, struct file *fp, struct sbuf *sb)
{
	struct file *curfp;
	struct pipepair *pair, *curpair;
	struct pipe_info info;
	int error, i;
	
	/* Get the pipe pair that the current pipe is in. */
	pair = ((struct pipe *) fp->f_data)->pipe_pair;

	/* Find out if we are the write end. */
	info.iswriteend = (&pair->pp_wpipe == fp->f_data);
	info.magic = SLS_PIPE_INFO_MAGIC;

	/* 
	 * Assume the other end is closed for now, if it 
	 * isn't we'll find it later. Also assume we are
	 * actually going to need to restore this (if we
	 * restore the other end, we don't need to do
	 * anything).
	 */
	info.onlyend = 1;
	info.needrest = 1;

	/* Find out if we are the read or the writeend. */

	/* 
	 * Pipes are a tricky case, because we have to find
	 * which two FDs correspond to its ends. This means
	 * we have to traverse the whole file descriptor space
	 * to find the other end.
	 */
	for (i = 0; i <= p->p_fd->fd_lastfile; i++) {
	    if (!fdisused(p->p_fd, i))
		continue;

	    curfp = p->p_fd->fd_files->fdt_ofiles[i].fde_file;

	    /* 
	     * If we found ourselves, return. It means the other
	     * end of the pipe is open. This in turn means that
	     * when we call sls_pipe_ckpt() for the other end,
	     * we'll find this end before it, and checkpoint 
	     * both of them at once.
	     */
	    if (curfp == fp) {
		info.needrest = 0;
		break;
	    }

	    /* 
	     * If the file pointer isn't a pipe, it's
	     * not what we're looking for.
	     */
	    if (curfp->f_type != DTYPE_PIPE)
		continue;

	    curpair = ((struct pipe *)curfp->f_data)->pipe_pair;

	    if (curpair == pair) {
		/* Got it! */
		info.onlyend = 0;
		info.otherend = i;
		break;
	    }
	}
	
	/* Write out the data. */
	error = sbuf_bcat(sb, (void *) &info, sizeof(info));
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_socket_ckpt(struct proc *p, struct socket *so, struct sbuf *sb)
{
	struct sock_info info;
	struct inpcb *inpcb;
	int error;

	inpcb = (struct inpcb *) so->so_pcb;

	/* 
	 * XXX Right now we're using a small subset of these 
	 * fields, but we are going to need them later. 
	 */
	info.family = so->so_proto->pr_domain->dom_family;
	info.proto = so->so_proto->pr_protocol;
	info.type = so->so_proto->pr_type;

	info.options = so->so_options;

	info.vflag = inpcb->inp_vflag;
	info.flags = inpcb->inp_flags;
	info.flags2 = inpcb->inp_flags2;
	info.ip_p = inpcb->inp_ip_p;
	
	info.inp_inc.inc_flags = inpcb->inp_inc.inc_flags;
	info.inp_inc.inc_len = inpcb->inp_inc.inc_len;
	info.inp_inc.inc_fibnum = inpcb->inp_inc.inc_fibnum;
	info.inp_inc.ie_fport = inpcb->inp_inc.inc_ie.ie_fport;
	info.inp_inc.ie_lport = inpcb->inp_inc.inc_ie.ie_lport;

	memcpy(info.inp_inc.ie_ufaddr, &inpcb->inp_inc.inc_faddr, 0x10);
	memcpy(info.inp_inc.ie_uladdr, &inpcb->inp_inc.inc_laddr, 0x10);

	error = sbuf_bcat(sb, (void *) &info, sizeof(info));
	if (error != 0)
	    return error;

	return 0;
}

/* Get generic file info applicable to all kinds of flies. */
int
sls_file_ckpt(struct proc *p, struct file *file, int fd, struct sbuf *sb)
{
	int error;
	struct file_info info;

	info.type = file->f_type;
	info.flag = file->f_flag;
	info.offset = file->f_offset;
	info.fd = fd;

	info.magic = SLS_FILE_INFO_MAGIC;

	error = sbuf_bcat(sb, (void *) &info, sizeof(info));

	return error;
}

static int
sls_vnode_rest(struct proc *p, struct sbuf *path, struct file_info *info)
{
	char *filepath;
	int error;

	filepath = sbuf_data(path);

	error = kern_openat(curthread, info->fd, filepath, 
		UIO_SYSSPACE, O_RDWR | O_CREAT, S_IRWXU);	
	if (error != 0)
	    return error;

	error = kern_lseek(curthread, info->fd, info->offset, SEEK_SET);
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_kqueue_rest(struct proc *p, struct kqueue_info *kqinfo, struct file_info *info)
{
	struct kevent *kev;
	int error, i;
	int kqfd;

	/* Create a kqueue for the process. */
	error = kern_kqueue(curthread, 0, NULL);
	if (error != 0)
	    return error;

	/* Move the fd to its original position. */
	kqfd = curthread->td_retval[0];
	if (kqfd != info->fd) {
	    error = kern_dup(curthread, FDDUP_FIXED, 0, kqfd, info->fd);
	    if (error != 0) {
		kern_close(curthread, kqfd);
		return error;
	    }

	    kern_close(curthread, kqfd);
	}

	/* For each kqinfo, create a kevent and register it. */
	for (i = 0; i < kqinfo->numevents; i++) {
	    kev = malloc(sizeof(*kev), M_SLSMM, M_WAITOK);

	    kev->ident = sls_kevents[i].ident;
	    /* XXX Right now we only need EVFILT_READ, which we 
	     * aren't getting at checkpoint time. Find out why.
	     */
	    kev->filter = sls_kevents[i].filter; 
	    kev->flags = sls_kevents[i].flags;
	    kev->fflags = sls_kevents[i].fflags;
	    kev->data = sls_kevents[i].data;
	    /* Add the kevetnt to the kqueue */
	    kev->flags = EV_ADD;

	    /* If any of the events were disabled , keep them that way. */
	    if ((sls_kevents[i].status & KN_DISABLED) != 0)
		kev->flags |= EV_DISABLE;

	    error = kqfd_register(kqfd, kev, curthread, 1);
	    if (error != 0) {
		/* 
		 * We don't handle restoring certain types of
		 * fds like IPv6 sockets yet.
		 */
		SLS_DBG("(BUG) fd for kevent %ld not restored\n", kev->ident);
	    }
	}

	return 0;
}

static int
sls_pipe_rest(struct proc *p, struct pipe_info *ppinfo, struct file_info *info)
{
	int filedes[2];
	int readend, writeend;
	int curfd;
	int error;

	/* 
	 * We may not need to restore this, because we 
	 * can do it later for the pipe's other end.
	 */
	if (ppinfo->needrest == 0)
	    return 0;

	error = kern_pipe(curthread, filedes, O_NONBLOCK, NULL, NULL);
	if (error != 0)
	    return error;

	/* If one end of the pipe is closed, have both ends point to the same fd. */
	if (info->fd == ppinfo->onlyend) {

	    if (ppinfo->iswriteend) {
		kern_close(curthread, filedes[1]);
		curfd = filedes[0];
	    } else {
		kern_close(curthread, filedes[0]);
		curfd = filedes[1];
	    }

	    if (info->fd != curfd) {
		error = kern_dup(curthread, FDDUP_FIXED, 0, curfd, info->fd);
		if (error != 0)
		    return error;

		kern_close(curthread, curfd);
	    }

	    kern_fcntl_freebsd(curthread, info->fd, F_SETFL, O_RDWR | O_NONBLOCK);
	} else {
	    /* Otherwise find which end is the write end. */
	    readend = (ppinfo->iswriteend) ? ppinfo->otherend : info->fd;
	    writeend = (ppinfo->iswriteend) ? info->fd : ppinfo->otherend;

	    /* Otherwise find which end is the write end. */
	    /* 
	     * Move the descriptors to the right place. 
	     * XXX What if the descriptors need to go to each other's place?
	     * We aren't handling this right now.
	     */

	    if (writeend != filedes[1]) {
		error = kern_dup(curthread, FDDUP_FIXED, 0, filedes[1], writeend);
		if (error != 0)
		    return error;

		kern_close(curthread, filedes[1]);
	    }

	    if (readend != filedes[0]) {
		error = kern_dup(curthread, FDDUP_FIXED, 0, filedes[0], readend);
		if (error != 0)
		    return error;

		kern_close(curthread, filedes[0]);
	    }

	    kern_fcntl_freebsd(curthread, readend, F_SETFL, O_RDWR | O_NONBLOCK);
	    kern_fcntl_freebsd(curthread, writeend, F_SETFL, O_RDWR | O_NONBLOCK);
	    
	}



	return 0;
}

static int
sls_socket_rest(struct proc *p, struct sock_info *info, struct file_info *finfo)
{
	struct sockaddr_in addr_in;
	struct sockaddr *sa;
	struct socket *so;
	struct file *fp;
	int sockfd;
	int error;

	/* Open the socket. */
	error = kern_socket(curthread, info->family, info->type, info->proto);
	if (error != 0)
	    return error;

	/* Move the socket to the correct position. */
	sockfd = curthread->td_retval[0];
	if (sockfd != finfo->fd) {
	    error = kern_dup(curthread, FDDUP_FIXED, 0, sockfd, finfo->fd);
	    if (error != 0) {
		kern_close(curthread, sockfd);
		return error;
	    }

	    kern_close(curthread, sockfd);
	    sockfd = finfo->fd;
	}

	/* Reach into the file descriptor and get the socket. */
	fp = curthread->td_proc->p_fd->fd_files->fdt_ofiles[sockfd].fde_file;
	so = (struct socket *) fp->f_data;

	/* Set up all the options again. */

	/* Do the same for the Internet protocol PCB. */
	//so->so_options = info->options;

	sa = malloc(SOCK_MAXADDRLEN, M_SLSMM, M_WAITOK);

	/* Zero the address structs. */
	bzero(&addr_in, sizeof(addr_in));
	bzero(sa, SOCK_MAXADDRLEN);

	addr_in.sin_family = info->family;
	addr_in.sin_port = info->inp_inc.ie_lport;
	/* XXX Find a way to transfer the address if it exists. */
	addr_in.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Create the generic address. */
	memcpy(sa, &addr_in, sizeof(addr_in));
	sa->sa_len = sizeof(addr_in);

	error = kern_bindat(curthread, AT_FDCWD, sockfd, sa);
	if (error != 0) {
	    kern_close(curthread, sockfd);
	    free(sa, M_SLSMM);
	    return error;
	}

	/* XXX Pick max backlog right now, find the actual one later. */
	error = kern_listen(curthread, sockfd, 511);
	if (error != 0) {
	    kern_close(curthread, sockfd);
	    free(sa, M_SLSMM);
	    return error;
	}

	kern_fcntl_freebsd(curthread, sockfd, F_SETFL, O_RDWR | O_NONBLOCK);

	free(sa, M_SLSMM);
	return 0;
}

int 
sls_file_rest(struct proc *p, void *data, struct file_info *info)
{
	switch(info->type) {
	case DTYPE_VNODE:
	    return sls_vnode_rest(p, (struct sbuf *) data, info);

	case DTYPE_KQUEUE:
	    return sls_kqueue_rest(p, (struct kqueue_info*) data, info);
	
	case DTYPE_PIPE:
	    return sls_pipe_rest(p, data, info);

	case DTYPE_SOCKET:
	    return sls_socket_rest(p, data, info);

	default:
	    panic("invalid file type");
	}

	return 0;
}

int
sls_filedesc_ckpt(struct proc *p, struct sbuf *sb)
{
	int i;
	int error = 0;
	struct file *fp;
	struct filedesc *filedesc;
	struct filedesc_info filedesc_info;
	struct file_info sentinel;
	struct socket *so;

	filedesc = p->p_fd;

	vhold(filedesc->fd_cdir);
	vhold(filedesc->fd_rdir);

	filedesc_info.fd_cmask = filedesc->fd_cmask;
	filedesc_info.num_files = 0;
	filedesc_info.magic = SLS_FILEDESC_INFO_MAGIC;

	FILEDESC_XLOCK(filedesc);

	error = sbuf_bcat(sb, (void *) &filedesc_info, sizeof(filedesc_info));
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


	for (i = 0; i <= filedesc->fd_lastfile; i++) {
	    if (!fdisused(filedesc, i))
		continue;

	    fp = filedesc->fd_files->fdt_ofiles[i].fde_file;

	    /* 
	     * XXX Right now we checkpoint a subset of all file types. 
	     * Depending on what type the file is, put in some extra
	     * checks to see if we should actually checkpoint it.
	     */
	    switch (fp->f_type) {
	    case DTYPE_VNODE:
		/* Handle only regular vnodes for now. */
		if (fp->f_vnode->v_type != VREG)
		    continue;
		break;

	    case DTYPE_KQUEUE:
		/* Kqueues are fine. */
		break;

	    case DTYPE_PIPE:
		/* Pipes are also fine. */
		break;
	    case DTYPE_SOCKET:
		so = (struct socket *) fp->f_data;

		/* Only restore listening IPv4 TCP sockets. */
		if (so->so_type != SOCK_STREAM)
		    continue;

		if (so->so_proto->pr_protocol != IPPROTO_TCP)
		    continue;

		if (so->so_proto->pr_domain->dom_family != AF_INET)
		    continue;

		break;

	    default:
		continue;

	    }

	    /* Checkpoint the file structure itself. */
	    error = sls_file_ckpt(p, fp, i, sb);
	    if (error != 0)
		goto done;

	    /* Checkpoint further information depending on the file's type. */
	    switch (fp->f_type) {
	    /* Backed by a vnode - get the name. */
	    case DTYPE_VNODE:
		error = sls_vnode_ckpt(p, fp->f_vnode, sb);
		break;

	    /* Backed by a kqueue - get all pending knotes. */
	    case DTYPE_KQUEUE:
		error = sls_kqueue_ckpt(p, fp, sb);
		break;

	    /* Backed by a pipe - get existing data. */
	    case DTYPE_PIPE:
		error = sls_pipe_ckpt(p, fp, sb);
		break;

	    case DTYPE_SOCKET:
		error = sls_socket_ckpt(p, (struct socket *) fp->f_data, sb);
		break;

	    default:
		panic("invalid file type");
	    }

	    if (error != 0)
		goto done;

	}

	memset(&sentinel, 0, sizeof(sentinel));
	sentinel.magic = SLS_FILES_END;
	error = sbuf_bcat(sb, (void *) &sentinel, sizeof(sentinel));
	if (error != 0)
	    goto done;


done:

	vdrop(filedesc->fd_cdir);
	vdrop(filedesc->fd_rdir);

	FILEDESC_XUNLOCK(filedesc);


	return error;
}

int
sls_filedesc_rest(struct proc *p, struct filedesc_info info)
{
	int stdfds[] = { 0, 1, 2 };
	struct filedesc *newfdp;
	char *cdir, *rdir;
	int error = 0;

	cdir = sbuf_data(info.cdir);
	rdir = sbuf_data(info.rdir);
	/* 
	 * We assume that fds 0, 1, and 2 are still stdin, stdout, stderr.
	 * This is a valid assumption considering that we control the process 
	 * on top of which we restore.
	 */
	error = fdcopy_remapped(p->p_fd, stdfds, 3, &newfdp);
	if (error != 0)
	    return error;
	fdinstall_remapped(curthread, newfdp);

	PROC_UNLOCK(p);
	error = kern_chdir(curthread, cdir, UIO_SYSSPACE);
	if (error != 0)
	    return error;

	error = kern_chroot(curthread, rdir, UIO_SYSSPACE);
	if (error != 0)
	    return error;
	PROC_LOCK(p);

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info.fd_cmask;
	FILEDESC_XUNLOCK(newfdp);

	return 0;
}
