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

#if 0
static int
slsckpt_sockbuf(struct sockbuf *sockbuf, struct slsckpt_data *sckpt_data)
{
	struct filedescent **fdescent;
	struct mbuf *packetm, *m;
	struct slsmbuf slsmbuf;
	struct sls_record *rec;
	size_t datalen, clen;
	struct cmsghdr *cm;
	struct sbuf *sb;
	int error, i;
	int numfds;

	sb = sbuf_new_auto();

	SOCKBUF_LOCK(sockbuf);
	/* 
	 * Iterate through all packets. We don't worry about
	 * denoting where each record ends, we can find out
	 * by reading the mbuf flags and checking for M_EOR.
	 */
	for (packetm = sockbuf->sb_mb; packetm != NULL; 
	    packetm = packetm->m_nextpkt) {

		for (m = packetm; m != NULL; m = m->m_next) {
			slsmbuf.magic = SLSMBUF_ID;
			slsmbuf.slsid = (uint64_t) sockbuf;
			/* We associate the mbufs with the socket buffer. */
			slsmbuf.type = packetm->m_type;
			slsmbuf.flags = packetm->m_flags;
			slsmbuf.len = packetm->m_len;

			/* Dump the metadata of the mbuf. */
			sbuf_bcat(sb, &slsmbuf, sizeof(slsmbuf));

			/* Get the data itself. */
			sbuf_bcat(sb, mtod(m, caddr_t), slsmbuf.len);

			/* 
			 * If we just dumped a UNIX SCM_RIGHTS message, also dump the
			 * filedescent it's pointing to, which holds pointers to the
			 * file descriptors and their capabilities.
			 */
			if (m->m_type == MT_CONTROL) {
				/* Seems like there can be multiple messages in a control packet. */
				cm = mtod(m, struct cmsghdr *);
				clen = cm->cmsg_len;

				while (cm != NULL) {
					/* We only care about passing file descriptors. */
					if ((cm->cmsg_level == SOL_SOCKET) &&
					    (cm->cmsg_type != SCM_RIGHTS)) {

						/* The data is a pointer to a bunch of filedescents. */
						fdescent = (struct filedescent **) CMSG_DATA(cm);
						datalen = (caddr_t) cm + cm->cmsg_len - (caddr_t) fdescent;
						numfds = datalen / sizeof(*fdescent);

						/* 
						 * Dump the array of filedescents into the record. 
						 * XXX This code makes the assumption that the file that
						 * is being transferred is still open in some table.
						 * This is not necessarily true, since the sender may
						 * close the fd after sending it and before the receiver
						 * gets the control message. We need to check the file table
						 * if the file has been checkpointed, and if not, to 
						 * checkpoint it ourselves.
						 */
						sbuf_bcat(sb, &numfds, sizeof(numfds));
						for (i = 0; i < numfds; i++)
							sbuf_bcat(sb, fdescent[i], sizeof(*fdescent[i]));
					}

					/* Check if there are any more messages in the mbuf. */
					if (CMSG_SPACE(datalen) < clen) {
						/* If so, go further into the array. */
						clen -= CMSG_SPACE(datalen);
						cm = (struct cmsghdr *) ((caddr_t) cm + CMSG_SPACE(datalen));
					} else {
						/* Otherwise we're done. */
						clen = 0;
						cm = NULL;
					}
				}
			}
		}
	}

	SOCKBUF_UNLOCK(sockbuf);
	error = sbuf_finish(sb);
	if (error != 0) {
		sbuf_delete(sb);
		return (error);
	}

	rec = sls_getrecord(sb, (uint64_t) slsmbuf.slsid, SLOSREC_SOCKBUF);
	/* Add the new buffer to the tables. */
	error = slskv_add(sckpt_data->sckpt_rectable, (uint64_t) sockbuf, (uintptr_t) rec);
	if (error != 0) {
		free(rec, M_SLSREC);
		sbuf_delete(sb);
		return (error);
	}

	return (0);
}
#endif

/* Get the address of a unix socket. */
static int
slsckpt_sock_un(struct socket *so, struct slssock *info)
{
	struct unpcb *unpcb;
	struct socket *sopeer;

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

int
slsckpt_socket(struct proc *p, struct socket *so, struct sbuf *sb,
    struct slsckpt_data *sckpt_data)
{
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
		error = slsckpt_sock_un(so, &info);
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
#if 0

	/* Get the rcv and snd buffers if not empty,  add their IDs to the socket. */

	if (so->so_rcv.sb_mbcnt != 0) {
		info.rcvid = (uint64_t) &so->so_rcv;
		error = slsckpt_sockbuf(&so->so_rcv, sckpt_data);
		if (error != 0)
			return (error);
	} else {
		info.rcvid = 0;
	}

	if (so->so_snd.sb_mbcnt != 0) {
		info.sndid = (uint64_t) &so->so_snd;
		error = slsckpt_sockbuf(&so->so_snd, sckpt_data);
		if (error != 0)
			return (error);
	} else {
		info.sndid = 0;
	}
#endif

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
slsrest_uipc_bindat(struct socket *so, struct sockaddr *name)
{
	struct thread *td = curthread;
	struct sockaddr_un *soun;
	struct nameidata nd;
	cap_rights_t rights;
	struct unpcb *unp;
	struct vnode *vp;
	int error;

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
	error = namei(&nd);
	vp = nd.ni_vp;
	NDFREE(&nd, NDF_ONLY_PNBUF);
	if (error != 0)
		return (error);
	/* XXX Make sure the refcounting is correct. */
	vref(vp);
	VOP_LOCK(vp, LK_EXCLUSIVE);
	/* Set up the internal vp state (used when calling connect()). */
	VOP_UNP_BIND(vp, unp);

	/* Finish intializing the socket itself. */
	unp->unp_vnode = vp;
	unp->unp_addr = (struct sockaddr_un *)sodupsockaddr(name, M_WAITOK);
	VOP_UNLOCK(vp, 0);

	return (0);
}

int
slsrest_socket(struct slsrest_data *restdata, struct slssock *info,
    struct slsfile *finfo, int *fdp)
{
	struct slskv_table *table = restdata->fptable;
	struct thread *td = curthread;
	struct sockaddr_un *unaddr;
	struct sockaddr_in *inaddr;
	struct socket *so, *sopeer;
	struct file *fp, *peerfp;
	struct unpcb *unpcb, *unpeerpcb;
	int fd, peerfd = -1;
	int option = 1;
	int family;
	int error;
	int mask;
	int i;

	/* We create an inet dummy sockets if we can't properly restore. */
	family = (info->family == AF_UNSPEC) ? AF_INET : info->family;

	/* Create the new socket. */
	error = kern_socket(td, family, info->type, info->proto);
	if (error != 0)
		return (error);

	fd = td->td_retval[0];

	/* Reach into the file descriptor and get the socket. */
	fp = FDTOFP(td->td_proc, fd);
	so = (struct socket *)fp->f_data;

	/* Restore the socket's nonblocking/async state. */
	if (info->family != AF_UNSPEC) {
		if ((info->state & SS_ASYNC) != 0)
			kern_fcntl_freebsd(td, fd, F_SETFL, O_ASYNC);
		if ((info->state & SS_NBIO) != 0)
			kern_fcntl_freebsd(td, fd, F_SETFL, O_NONBLOCK);
		so->so_options = info->options & (~SO_ACCEPTCONN);
	}

	/* Set any options we can. */
	if (info->options & SLS_SO_RESTORABLE) {
		for (i = 0; i < sizeof(int) * 8; i++) {
			mask = 1 << i;
			if ((mask & SLS_SO_RESTORABLE) == 0)
				continue;

			error = kern_setsockopt(td, fd, SOL_SOCKET, mask,
			    &option, UIO_SYSSPACE, sizeof(option));
			if (error != 0)
				goto error;
		}
	}

	DEBUG1("Restoring socket of family %d", info->family);
	switch (info->family) {
	/* A socket we can't restore. Mark it as closed. */
	case AF_UNSPEC:
		break;

	case AF_INET:
		inaddr = (struct sockaddr_in *)&info->in;

		/* Check if the socket is bound. */
		if (info->bound == 0)
			break;

		/*
		 * If this is a listening socket for a Metropolis function,
		 * assign it a random port, much like accept() does.
		 *  Try randomly picking a number from 1024 to 65535
		 */
		if (restdata->slsmetr.slsmetr_sockid == info->slsid) {

			for (i = 0; i < METROPOLIS_RETRIES; i++) {
				inaddr->sin_port = 1024 +
				    (random() % (65545 - 1024));
				error = kern_bindat(td, AT_FDCWD, fd,
				    (struct sockaddr *)inaddr);
				if (error == 0)
					break;
			}

			if (error != 0)
				goto error;

		} else {
			/*
			 * We use bind() instead of setting the address directly
			 * because we need to let the kernel know we are
			 * reserving the address.
			 */
			error = kern_bindat(
			    td, AT_FDCWD, fd, (struct sockaddr *)inaddr);
			if (error != 0)
				goto error;
		}

		break;

	case AF_LOCAL:
		unaddr = (struct sockaddr_un *)&info->un;
		/*
		 * Check if the socket has a peer. If it does, then it is
		 * either a UNIX stream data socket, or it is a socket
		 * created using socketpair(); in both cases, it has a peer,
		 * so we create it alongside it.
		 */
		if (info->unpeer != 0) {
			/* Create the socket peer. */
			error = kern_socket(
			    td, info->family, info->type, info->proto);
			if (error != 0)
				goto error;

			peerfd = td->td_retval[0];
			peerfp = FDTOFP(td->td_proc, peerfd);
			sopeer = (struct socket *)peerfp->f_data;

			/* Connect the two sockets. See kern_socketpair(). */
			error = soconnect2(so, sopeer);
			if (error != 0)
				goto error;

			if (info->type == SOCK_DGRAM) {
				/*
				 * Datagram connections are asymetric, so
				 * repeat the process to the other direction.
				 */
				error = soconnect2(sopeer, so);
				if (error != 0)
					goto error;
			} else if (so->so_proto->pr_flags & PR_CONNREQUIRED) {
				/* Use credentials to make the stream socket
				 * exclusive to the peer. */
				unpcb = sotounpcb(so);
				unpeerpcb = sotounpcb(sopeer);
				unp_copy_peercred(td, unpcb, unpeerpcb, unpcb);
			}

			break;
		}

		/*
		 * We assume it is either a listening socket, or a
		 * datagram socket. The only case where we don't bind
		 * is if it is a datagram client socket, in which case
		 * we just leave it be.
		 */
		if (info->bound == 0)
			break;

		/*
		 * The bind() function for UNIX sockets makes assumptions
		 * that do not hold here, so we use our own custom version.
		 */
		error = slsrest_uipc_bindat(so, (struct sockaddr *)unaddr);
		if (error != 0) {
			kern_close(td, fd);
			return (error);
		}

		break;

	default:
		panic(
		    "%s: Unknown protocol family %d\n", __func__, info->family);
	}

	/* Check if we need to listen for incoming connections. */
	if ((info->options & SO_ACCEPTCONN) != 0) {
		DEBUG("Setting the socket to listen");
		error = kern_listen(td, fd, info->backlog);
		if (error != 0)
			goto error;
	}

	if (peerfd >= 0) {
		error = slskv_add(table, info->unpeer, (uintptr_t)peerfp);
		if (error != 0)
			goto error;

		/* Get a hold for the table, before closing the fd. */
		if (!fhold(peerfp)) {
			error = EBADF;
			goto error;
		}

		kern_close(td, peerfd);
	}

	*fdp = fd;

#if 0
	/* Restore the data into the socket buffers. */
	error = slsrest_sockbuf(sockbuftable, info->rcvid, &so->so_rcv);
	if (error != 0)
		goto error;

	error = slsrest_sockbuf(sockbuftable, info->sndid, &so->so_snd);
	if (error != 0)
		goto error;

	/* If we also created our peer, restore its buffers, too. */
	if (sopeer != NULL) {
		error = slsrest_sockbuf(sockbuftable, info->peer_rcvid, &sopeer->so_rcv);
		if (error != 0)
			goto error;

		error = slsrest_sockbuf(sockbuftable, info->peer_sndid, &sopeer->so_snd);
		if (error != 0)
			goto error;
	}
#endif

	return (0);

error:
	if (peerfd >= 0)
		kern_close(td, peerfd);
	kern_close(td, fd);

	return (error);
}
