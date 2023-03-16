#include <sys/param.h>
#include <sys/selinfo.h>
#include <sys/domain.h>
#include <sys/endian.h>
#include <sys/event.h>
#include <sys/fcntl.h>
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
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/un.h>
#include <sys/unistd.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

#include <machine/param.h>

/* XXX Pipe has to be after selinfo */
#include <sys/pipe.h>

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
#include <slos_inode.h>
#include <sls_data.h>

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"
#include "sls_vnode.h"

#define METROPOLIS_RETRIES (1000)

/*
 * XXX Sendfile doesn't work, because there might be pageins in progress that
 * cannot be tracked across restarts. We could fix this by monitoring pageins
 * in progress from SLSFS, and restarting them together with the checkpoint.
 */

/*
 * XXX Fix restoring the socket buffers for all kinds of sockets. While we don't
 * restore TCP connection sockets, and we can drop UDP packets since UDP is not
 * reliable, Unix sockets assume that no data between sockets is lost.
 */

/*
 * These socket flags are just set and unset in sosetopt, without any side
 * effects. They are thus easily restorable.
 */
#define SLS_SO_RESTORABLE                                                  \
	(SO_DEBUG | SO_KEEPALIVE | SO_DONTROUTE | SO_USELOOPBACK |         \
	    SO_BROADCAST | SO_REUSEADDR | SO_REUSEPORT | SO_REUSEPORT_LB | \
	    SO_OOBINLINE | SO_TIMESTAMP | SO_BINTIME | SO_NOSIGPIPE |      \
	    SO_NO_DDP | SO_NO_OFFLOAD)


/* Get the address of a unix socket. */
static int
slsckpt_sock_un(
    struct socket *so, struct slssock *info, struct slsckpt_data *sckpt_data)
{
	struct unpcb *unpcb;
	struct socket *sopeer;
	int error;

	unpcb = sotounpcb(so);
	if (unpcb->unp_addr != NULL)
		memcpy(&info->un, unpcb->unp_addr, sizeof(info->un));

	/*
	 * If we are a data socket, get the peer. There are
	 * two possibilities: Either we are a stream data socket
	 * or a datagram socket created using socketpair(), in which
	 * case we do have a pair, or we are a listening stream socket
	 * or a named datagram socket, in which case we don't.
	 */
	if (unpcb->unp_conn != NULL) {
		sopeer = unpcb->unp_conn->unp_socket;
		info->unpeer = (uint64_t)sopeer;
		info->peer_rcvid = (uint64_t)&sopeer->so_rcv;
		info->peer_sndid = (uint64_t)&sopeer->so_snd;
	}
	info->bound = (unpcb->unp_vnode != NULL) ? 1 : 0;

	/* Checkpoint the vnode. */
	if (info->bound) {
		error = slsckpt_vnode(unpcb->unp_vnode, sckpt_data);
		if (error != 0)
			return (error);
		info->vnode = (uint64_t)unpcb->unp_vnode;
	}

	return (0);
}

/* Get the address of an inet socket. */
static int
slsckpt_sock_in(struct socket *so, struct slssock *info)
{
	struct inpcb *inpcb;

	inpcb = (struct inpcb *)so->so_pcb;

	/* Get the local address. */
	info->in.sin_family = AF_INET;
	info->in.sin_port = inpcb->inp_inc.inc_ie.ie_lport;
	info->in.sin_addr.s_addr = inpcb->inp_inc.inc_laddr.s_addr;
	info->in.sin_len = sizeof(struct sockaddr_in);

	/* Get general connection info. */
	info->in_conninfo = inpcb->inp_inc;
	info->bound = (inpcb->inp_flags & INP_INHASHLIST) ? 1 : 0;

	/* INET sockets can never be anonymous, so they have no peers. */

	return (0);
}

static int
slssock_checkpoint(
    struct file *fp, struct sbuf *sb, struct slsckpt_data *sckpt_data)
{
	struct socket *so = (struct socket *)fp->f_data;
	struct slssock info;
	int error;

	/* Zero out all fields. */
	memset(&info, 0, sizeof(info));

	info.magic = SLSSOCKET_ID;
	info.slsid = (uint64_t)so;

	/* Generic information about the socket type. */
	info.family = so->so_proto->pr_domain->dom_family;
	info.proto = so->so_proto->pr_protocol;
	info.type = so->so_proto->pr_type;
	info.state = so->so_state;
	info.options = so->so_options;

	/* Get the address depending on the type. */
	switch (info.family) {
	case AF_INET:
		error = slsckpt_sock_in(so, &info);
		break;

	case AF_LOCAL:
		error = slsckpt_sock_un(so, &info, sckpt_data);
		break;

	default:
		panic(
		    "%s: Unknown protocol family %d\n", __func__, info.family);
	}

	if (error != 0)
		return (error);

	switch (info.type) {
	case SOCK_STREAM:
	case SOCK_SEQPACKET:
		info.backlog = so->sol_qlimit;
		break;

	case SOCK_DGRAM:
	case SOCK_RAW:
	case SOCK_RDM:
		break;

	default:
		panic("%s: Unknown protocol type %d\n", __func__, info.type);
	}

	/*
	 * If this isn't a listening socket, mark it as invalid, because we
	 * can't restore it yet.
	 */
	if ((info.family == AF_INET) && (info.type == SOCK_STREAM) &&
	    !SOLISTENING(so))
		info.family = AF_UNSPEC;

	/* Write it out to the SLS record. */
	error = sbuf_bcat(sb, (void *)&info, sizeof(info));
	if (error != 0)
		return (error);

	return (0);
}

static int
slsrest_sockbuf(
    struct slskv_table *table, uint64_t sockbufid, struct sockbuf *sb)
{
	struct mbuf *m, *recm;
	int error;

	/* If the buffer is empty, we don't need to do anything else. */
	if (sockbufid == 0)
		return (0);

	/*
	 * Associate it with the buffer - it should be fully built already.
	 * Directly set up the sockbuf, we can't risk getting side-effects
	 * e.g. by internalizing MT_CONTROL, SCM_RIGHTS messages.
	 */
	error = slskv_find(table, sockbufid, (uintptr_t *)&m);
	if (error != 0)
		return (error);

	/* If we had no data, we're done. */
	if (m == NULL)
		return (0);

	sb->sb_mb = m;
	sb->sb_mbtail = m_last(m);

	/* Adjust sockbuf state for the new mbuf chain. */
	for (recm = m; recm != NULL; recm = m->m_nextpkt) {
		/* The final value of the field will be that of the final
		 * record. */
		sb->sb_lastrecord = recm;

		/* Adjust bookkeeping.*/
		sballoc(sb, m);
	}

	return (0);
}

/*
 * Modified version of uipc_bindat() that supposes that the file used for the
 * socket already exists (the original function assumes the opposite). The
 * function is also greatly simplified by the fact that the socket is also
 * visible to us, so there are no races (XXX there _are_ new races that arise
 * from the fact that we are binding to an existing file, but we assume
 * that there is no interference from outside the SLS for now while restoring).
 */
static int
slsrest_uipc_bindat(struct socket *so, struct sockaddr *name, struct vnode *vp)
{
	struct thread *td = curthread;
	struct sockaddr_un *soun;
	struct nameidata nd;
	cap_rights_t rights;
	struct unpcb *unp;

	/*
	 * Checks in the original function are turned into KASSERTs, because
	 * in our case we know only we can see the new socket, so a lot of
	 * edge cases are impossible unless something is horribly wrong.
	 */
	KASSERT((name->sa_family == AF_UNIX), ("socket is not unix socket"));

	unp = sotounpcb(so);
	KASSERT(unp != NULL, ("uipc_bind: unp == NULL"));

	soun = (struct sockaddr_un *)name;

	KASSERT((soun->sun_len <= sizeof(struct sockaddr_un)),
	    ("socket path size too large"));
	KASSERT((soun->sun_len > offsetof(struct sockaddr_un, sun_path)),
	    ("socket path size too small"));

	KASSERT(((unp->unp_flags & UNP_BINDING) == 0),
	    ("unix socket already binding"));
	KASSERT((unp->unp_vnode == NULL), ("unix socket already bound"));

	NDINIT_ATRIGHTS(&nd, LOOKUP, FOLLOW | AUDITVNODE1, UIO_SYSSPACE,
	    soun->sun_path, AT_FDCWD, &rights, td);

	/*
	 * The path in the socket address is relative to the current
	 * working directory of the process that opened it before checkpointing.
	 * (As a note, this scheme seems problematic since the open socket
	 * file can be used by two processes, each with a different working
	 * directory). For this reason we don't use the filepath in the socket,
	 * but directly restore the underlying vnode using Aurora.
	 */

	/* XXX Make sure the refcounting is correct. */
	VOP_LOCK(vp, LK_EXCLUSIVE);
	vref(vp);
	/* Set up the internal vp state (used when calling connect()). */
	VOP_UNP_BIND(vp, unp);

	/* Finish intializing the socket itself. */
	unp->unp_vnode = vp;
	unp->unp_addr = (struct sockaddr_un *)sodupsockaddr(name, M_WAITOK);
	VOP_UNLOCK(vp, 0);

	return (0);
}

/*
 * Restore any state set by fcntl.
 */
static int
slssock_fcntl(int fd, struct socket *so, struct slssock *slssock)
{
	struct thread *td = curthread;

	if (slssock->family != AF_UNSPEC) {
		if ((slssock->state & SS_ASYNC) != 0)
			kern_fcntl_freebsd(td, fd, F_SETFL, O_ASYNC);
		if ((slssock->state & SS_NBIO) != 0)
			kern_fcntl_freebsd(td, fd, F_SETFL, O_NONBLOCK);
		so->so_options = slssock->options & (~SO_ACCEPTCONN);
	}

	return (0);
}

/*
 * Set all restorable options.
 */
static int
slssock_setopt(int fd, struct slssock *slssock)
{
	int mask;
	const int intbits = sizeof(mask) * 8;
	struct thread *td = curthread;
	int option = 1;
	int error;
	int i;

	/* Set any options we can. */
	if ((slssock->options & SLS_SO_RESTORABLE) == 0)
		return (0);

	for (i = 0; i < intbits; i++) {
		mask = 1 << i;
		if ((mask & SLS_SO_RESTORABLE) == 0)
			continue;

		error = kern_setsockopt(td, fd, SOL_SOCKET, mask, &option,
		    UIO_SYSSPACE, sizeof(option));
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slssock_restore_afinet(struct slssock *slssock, int fd, uint64_t slsmetr_sockid)
{
	struct sockaddr_in *inaddr = (struct sockaddr_in *)&slssock->in;
	struct thread *td = curthread;
	int error, i;

	/* Check if the socket is bound. */
	if (slssock->bound == 0)
		return (0);

	/*
	 * XXXHACK If this is a listening socket for a Metropolis function,
	 * assign it a random port, much like accept() does.
	 *  Try randomly picking a number from 1024 to 65535
	 *
	 *  TODO: Every time accept() gets called from this special
	 *  socket the Metropolis function goes into hibernation, waiting for
	 *  a warm invocation. Metropolis cleans up unused functions after a
	 *  timeout.
	 */
	if (slsmetr_sockid == slssock->slsid) {

		for (i = 0; i < METROPOLIS_RETRIES; i++) {
			inaddr->sin_port = 1024 + (random() % (65545 - 1024));
			error = kern_bindat(
			    td, AT_FDCWD, fd, (struct sockaddr *)inaddr);
			if (error == 0)
				return (error);
		}

		if (error != 0)
			return (error);

	} else {
		/*
		 * We use bind() instead of setting the address directly
		 * because we need to let the kernel know we are
		 * reserving the address.
		 */
		error = kern_bindat(
		    td, AT_FDCWD, fd, (struct sockaddr *)inaddr);
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slssock_restore_pair(
    struct slsrest_data *restdata, struct socket *so, struct slssock *slssock)
{
	struct unpcb *unpcb, *unpeerpcb;
	struct thread *td = curthread;
	struct socket *sopeer;
	struct file *peerfp;
	int peerfd;
	int error;

	/* Create the socket peer. */
	error = kern_socket(td, slssock->family, slssock->type, slssock->proto);
	if (error != 0)
		return (error);

	peerfd = td->td_retval[0];
	error = slsfile_extractfp(peerfd, &peerfp);
	if (error != 0) {
		slsfile_attemptclose(peerfd);
		return (error);
	}

	sopeer = (struct socket *)peerfp->f_data;

	error = slskv_add(
	    restdata->fptable, slssock->unpeer, (uintptr_t)peerfp);
	if (error != 0) {
		fdrop(peerfp, td);
		return error;
	}

	/* Connect the two sockets. See kern_socketpair(). */
	error = soconnect2(so, sopeer);
	if (error != 0)
		return (error);

	if (slssock->type == SOCK_DGRAM) {
		/*
		 * Datagram connections are asymetric, so
		 * repeat the process to the other direction.
		 */
		error = soconnect2(sopeer, so);
		if (error != 0)
			return (error);
	} else if (so->so_proto->pr_flags & PR_CONNREQUIRED) {
		/* Use credentials to make the stream socket
		 * exclusive to the peer. */
		unpcb = sotounpcb(so);
		unpeerpcb = sotounpcb(sopeer);
		unp_copy_peercred(td, unpcb, unpeerpcb, unpcb);
	}

	return (0);
}

static int
slssock_restore_aflocal(
    struct slsrest_data *restdata, struct socket *so, struct slssock *slssock)
{
	struct sockaddr_un *unaddr = (struct sockaddr_un *)&slssock->un;
	struct vnode *vp;
	int error;

	/*
	 * We assume it is either a listening socket, or a
	 * datagram socket. The only case where we don't bind
	 * is if it is a datagram client socket, in which case
	 * we just leave it be.
	 */
	if (slssock->bound == 0)
		goto out;

	/*
	 * Find the checkpoint time working directory to properly resolve
	 * UNIX socket names.
	 */
	error = slskv_find(
	    restdata->sckpt->sckpt_vntable, slssock->vnode, (uintptr_t *)&vp);
	if (error != 0)
		return (error);

	/*
	 * The bind() function for UNIX sockets makes assumptions
	 * that do not hold here, so we use our own custom version.
	 */
	error = slsrest_uipc_bindat(so, (struct sockaddr *)unaddr, vp);
	if (error != 0)
		return (error);

	/*
	 * Check if the socket has a peer. If it does, then it is
	 * either a UNIX stream data socket, or it is a socket
	 * created using socketpair(); in both cases, it has a peer,
	 * so we create it alongside it.
	 */
	if (slssock->unpeer != 0)
		return (slssock_restore_pair(restdata, so, slssock));

out:

	return (0);
}

static int
slssock_restore(void *slsbacker, struct slsfile *finfo,
    struct slsrest_data *restdata, struct file **fpp)
{
	struct slssock *slssock = (struct slssock *)slsbacker;
	struct thread *td = curthread;
	int close_error, error;
	int fd, peerfd = -1;
	struct socket *so;
	struct file *fp;
	uintptr_t peer;
	int family;

	/* Same as with pipes, check if we have already restored it. */
	if (slskv_find(restdata->fptable, finfo->slsid, &peer) == 0) {
		*fpp = NULL;
		return (0);
	}

	/* We create an inet dummy sockets if we can't properly restore. */
	family = (slssock->family == AF_UNSPEC) ? AF_INET : slssock->family;

	/* Create the new socket. */
	error = kern_socket(td, family, slssock->type, slssock->proto);
	if (error != 0)
		return (error);
	fd = td->td_retval[0];
	so = (struct socket *)FDTOFP(td->td_proc, fd)->f_data;

	/* Use setsockopt() to restore any state */
	error = slssock_setopt(fd, slssock);
	if (error != 0)
		goto error;

	/* Use fcntl to set up async/nonblocking state. */
	error = slssock_fcntl(fd, so, slssock);
	if (error != 0)
		goto error;

	DEBUG1("Restoring socket of family %d", slssock->family);
	switch (slssock->family) {
	/* A socket we can't restore. Mark it as closed. */
	case AF_UNSPEC:
		break;

	case AF_INET:
		error = slssock_restore_afinet(
		    slssock, fd, restdata->slsmetr.slsmetr_sockid);
		if (error != 0)
			goto error;
		break;

	case AF_LOCAL:
		error = slssock_restore_aflocal(restdata, so, slssock);
		if (error != 0)
			goto error;
		break;

	default:
		slsfile_attemptclose(fd);
		DEBUG2("%s: Unknown protocol %d\n", __func__, slssock->family);
		return (EINVAL);
	}

	/* Check if we need to listen for incoming connections. */
	if ((slssock->options & SO_ACCEPTCONN) != 0) {
		DEBUG("Setting the socket to listen");
		error = kern_listen(td, fd, slssock->backlog);
		if (error != 0)
			goto error;
	}

	/* The rest of the operations are done directly on the descriptor. */
	error = slsfile_extractfp(fd, &fp);
	if (error != 0)
		goto error;
	fd = -1;

	*fpp = fp;

	/* XXX Restore the send/receive socket buffers. */

	return (0);

error:
	if (fd >= 0) {
		close_error = kern_close(td, fd);
		if (close_error != 0)
			DEBUG1("Error %d when closing file", close_error);

		return (error);
	}

	if (peerfd >= 0) {
		close_error = kern_close(td, peerfd);
		if (close_error != 0)
			DEBUG1("Error %d when closing file", close_error);
	}

	return (error);
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

	return (uint64_t)sopeer;
}

static int
slssock_slsid(struct file *fp, uint64_t *slsidp)
{
	uint64_t slsid, peerid;

	slsid = (uint64_t)fp->f_data;

	/* Get the ID of the peer socket, or 0 if none. */
	peerid = slsckpt_getsocket_peer((struct socket *)(fp->f_data));
	*slsidp = max(slsid, peerid);

	return (0);
}

static bool
slssock_supported(struct file *fp)
{
	struct socket *so = (struct socket *)fp->f_data;

	/* IPv4 and UNIX sockets are allowed. */
	if (so->so_proto->pr_domain->dom_family == AF_UNIX)
		return (true);

	/* XXX Do the fast thing for connected sockets for now. */
	if (so->so_proto->pr_domain->dom_family == AF_INET) {
		if (so->so_type == SOCK_STREAM)
			return (SOLISTENING(so));
		return (true);
	}

	/* Anything else isn't. */
	return (false);

	/*
	 * XXX Protocols we might need to support in the future:
	 * AF_INET6, AF_NETGRAPH. If we put networking-focused
	 * applications in the SLS, also AF_ARP, AF_LINK maybe?
	 */
}

struct slsfile_ops slssock_ops = {
	.slsfile_supported = slssock_supported,
	.slsfile_slsid = slssock_slsid,
	.slsfile_checkpoint = slssock_checkpoint,
	.slsfile_restore = slssock_restore,
};
