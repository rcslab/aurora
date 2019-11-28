#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

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

#include <slos.h>
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_path.h"

#include "imported_sls.h"

int
slsckpt_socket(struct proc *p, struct socket *so, struct sbuf *sb)
{
	struct slssock info;
	struct inpcb *inpcb;
	int error;

	inpcb = (struct inpcb *) so->so_pcb;

	/* 
	 * XXX Right now we're using a small subset of these 
	 * fields, but we are going to need them later. 
	 */
	info.magic = SLSSOCKET_ID;
	info.slsid = (uint64_t) so;

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

int
slsrest_socket(struct slssock *info, struct slsfile *finfo, int *fdp)
{
	struct sockaddr_in addr_in;
	struct sockaddr *sa;
	struct socket *so;
	struct file *fp;
	int fd;
	int error;

	/* Open the socket. */
	error = kern_socket(curthread, info->family, info->type, info->proto);
	if (error != 0)
	    return error;

	/* Move the socket to the correct position. */
	fd = curthread->td_retval[0];

	/* Reach into the file descriptor and get the socket. */
	fp = curthread->td_proc->p_fd->fd_files->fdt_ofiles[fd].fde_file;
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

	error = kern_bindat(curthread, AT_FDCWD, fd, sa);
	if (error != 0) {
	    kern_close(curthread, fd);
	    free(sa, M_SLSMM);
	    return error;
	}

	if (info->type == SOCK_STREAM) {
	    /* XXX Pick max backlog right now, find the actual one later. */
	    error = kern_listen(curthread, fd, 511);
	    if (error != 0) {
		kern_close(curthread, fd);
		free(sa, M_SLSMM);
		return error;
	    }

	}
	kern_fcntl_freebsd(curthread, fd, F_SETFL, O_RDWR | O_NONBLOCK);
	free(sa, M_SLSMM);

	*fdp = fd;

	return 0;
}
