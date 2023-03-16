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

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"

static int
slspipe_checkpoint(
    struct file *fp, struct sbuf *sb, struct slsckpt_data *sckpt_data)
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

/*
 * Restore an end of the pipe pair.
 */
static int
slspipe_restore_pipeend(struct pipe *pp, struct slspipe *slspipe)
{

	/* Restore the buffer's state. */
	pp->pipe_buffer.cnt = slspipe->pipebuf.cnt;
	pp->pipe_buffer.in = slspipe->pipebuf.in;
	pp->pipe_buffer.out = slspipe->pipebuf.out;
	/* Check if the data fits in the newly created pipe. */
	if (pp->pipe_buffer.size < slspipe->pipebuf.cnt)
		return (EINVAL);

	memcpy(pp->pipe_buffer.buffer, slspipe->data, slspipe->pipebuf.cnt);

	return (0);
}

static int
slspipe_restore(void *slsbacker, struct slsfile *info,
    struct slsrest_data *restdata, struct file **fpp)
{
	struct slspipe *slspipe = (struct slspipe *)slsbacker;
	struct file *localfp = NULL, *peerfp = NULL;
	uint64_t slsid = slspipe->slsid;
	struct thread *td = curthread;
	int localfd, peerfd;
	int close_error;
	struct pipe *pp;
	int filedes[2];
	int error;

	/* The pipe has been created, it just needs to be initialized. */
	if (slskv_find(restdata->fptable, slsid, (uintptr_t *)&peerfp) == 0) {
		pp = (struct pipe *)peerfp->f_data;

		error = slspipe_restore_pipeend(pp, slspipe);
		if (error != 0)
			return (error);

		/* The pipe end is already in the table. */
		*fpp = NULL;

		return (0);
	}

	/* Create both ends of the pipe. */
	error = kern_pipe(curthread, filedes,
	    info->flag & (O_NONBLOCK | O_CLOEXEC), NULL, NULL);
	if (error != 0)
		return error;

	/* Check whether we are the read or the write end. */
	if (slspipe->iswriteend) {
		localfd = filedes[1];
		peerfd = filedes[0];
	} else {
		localfd = filedes[0];
		peerfd = filedes[1];
	}

	/* Extract the local and remote file descriptors from the table. */
	error = slsfile_extractfp(localfd, &localfp);
	if (error != 0)
		goto error;

	/* We have consumed the fd, ignore during cleanup. */
	localfd = -1;

	error = slsfile_extractfp(peerfd, &peerfp);
	if (error != 0)
		goto error;

	/* Same as with localfd.*/
	peerfd = -1;

	/*
	 * Restore the local pipe state.
	 */
	pp = (struct pipe *)localfp->f_data;
	error = slspipe_restore_pipeend(pp, slspipe);
	if (error != 0)
		goto error;

	/*
	 * We take the liberty here of using the pipe's SLS ID instead
	 * of the file pointer. Since we have made it so at checkpoint
	 * time that pipes and their corresponding open files have the
	 * same ID, this is not a problem.
	 */
	error = slskv_add(restdata->fptable, slspipe->peer, (uintptr_t)peerfp);
	if (error != 0)
		goto error;

	/* The caller adds the local descriptor to the table. */
	*fpp = localfp;

	return (0);

error:
	if (localfd >= 0) {
		close_error = kern_close(td, localfd);
		if (close_error != 0)
			DEBUG1("kern_close failed with %d", error);
	}

	if (peerfd >= 0) {
		close_error = kern_close(td, peerfd);
		if (close_error != 0)
			DEBUG1("kern_close failed with %d", error);
	}

	if (localfp != NULL)
		fdrop(localfp, td);

	if (peerfp != NULL)
		fdrop(peerfp, td);

	return (error);
}

static int
slspipe_slsid(struct file *fp, uint64_t *slsidp)
{
	/* Use the pipe ID because each end has its own record. */
	*slsidp = (uint64_t)fp->f_data;

	return (0);
}

static bool
slspipe_supported(struct file *fp)
{
	return (true);
}

struct slsfile_ops slspipe_ops = {
	.slsfile_supported = slspipe_supported,
	.slsfile_slsid = slspipe_slsid,
	.slsfile_checkpoint = slspipe_checkpoint,
	.slsfile_restore = slspipe_restore,
};
