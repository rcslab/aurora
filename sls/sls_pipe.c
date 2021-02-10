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
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/unistd.h>
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
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"

int
slsckpt_pipe(struct proc *p, struct file *fp, struct sbuf *sb)
{
	struct pipe *pipe, *peer;
	struct slspipe info;
	int error;

	/* Get the current pipe and its peer. */
	pipe = (struct pipe *)fp->f_data;
	peer = pipe->pipe_peer;

	/* Find out if we are the write end. */
	info.iswriteend = (pipe == &pipe->pipe_pair->pp_wpipe);

	info.magic = SLSPIPE_ID;
	info.slsid = (uint64_t)pipe;
	info.pipebuf = pipe->pipe_buffer;

	/*
	 * We use the peer's address regardless of
	 * whether it's actually open. If it is,
	 * we will find it elsewhere, and create
	 * a record for it.
	 */
	info.peer = (uint64_t)peer;

	/* Write out the data. */
	error = sbuf_bcat(sb, (void *)&info, sizeof(info));
	if (error != 0)
		return (error);

	error = sbuf_bcat(sb, pipe->pipe_buffer.buffer, pipe->pipe_buffer.cnt);
	if (error != 0)
		return (error);

	/* XXX Account for pipe direct mappings */

	return (0);
}

int
slsrest_pipe(
    struct slskv_table *fptable, int flags, struct slspipe *ppinfo, int *fdp)
{
	struct file *fp, *peerfp;
	int localfd, peerfd;
	struct pipe *pipe;
	int filedes[2];
	int error;

	/* Create both ends of the pipe. */
	error = kern_pipe(curthread, filedes, flags, NULL, NULL);
	if (error != 0)
		return error;

	/* Check whether we are the read or the write end. */
	if (ppinfo->iswriteend) {
		localfd = filedes[1];
		peerfd = filedes[0];
	} else {
		localfd = filedes[0];
		peerfd = filedes[1];
	}

	fp = FDTOFP(curthread->td_proc, localfd);
	pipe = (struct pipe *)fp->f_data;

	/* Restore the buffer's state. */
	pipe->pipe_buffer.cnt = ppinfo->pipebuf.cnt;
	pipe->pipe_buffer.in = ppinfo->pipebuf.in;
	pipe->pipe_buffer.out = ppinfo->pipebuf.out;
	/* Check if the data fits in the newly created pipe. */
	if (pipe->pipe_buffer.size < ppinfo->pipebuf.cnt)
		return (EINVAL);

	memcpy(pipe->pipe_buffer.buffer, ppinfo->data, ppinfo->pipebuf.cnt);

	/*
	 * Grab the peer's file pointer, save it to the table.
	 * When we come across the peer's record later, we'll have
	 * already restored it, and so we will just ignore it. We
	 * thus avoid restoring the same pipe twice.
	 */
	peerfp = FDTOFP(curthread->td_proc, peerfd);

	/*
	 * We take the liberty here of using the pipe's SLS ID instead
	 * of the file pointer. Since we have made it so at checkpoint
	 * time that pipes and their corresponding open files have the
	 * same ID, this is not a problem.
	 */
	error = slskv_add(fptable, ppinfo->peer, (uintptr_t)peerfp);
	if (error != 0) {
		kern_close(curthread, localfd);
		kern_close(curthread, peerfd);
		return (error);
	}

	/* Get a reference on behalf of the hashtable. */
	if (!fhold(peerfp)) {
		kern_close(curthread, peerfd);
		return (EBADF);
	}

	/* Remove it from this process and this fd. */
	kern_close(curthread, peerfd);

	/* The caller will take care of the local file descriptor the same way.
	 */
	*fdp = localfd;

	return (0);
}
