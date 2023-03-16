#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/selinfo.h>
#include <sys/conf.h>
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
#include <sys/tty.h>
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
#include <sls_data.h>

#include "pts_internal.h"
#include "sls_file.h"
#include "sls_internal.h"

/*
 * Variant of ttyoutq_read() that nondestructively reads all data from
 * the input queue into an sbuf.
 */
static int
slsckpt_ttyinq_read(struct ttyinq *ti, struct sbuf *ptssb)
{
	size_t curbegin, curend;
	struct ttyinq_block *tib;
	size_t cbegin, cend, clen;
	size_t copied = 0;
	size_t datasize;
	struct sbuf *sb;
	int error = 0;
	void *data;

	sb = sbuf_new_auto();

	/* We don't want to modify the queue itself, so we use our own indices.
	 */
	curbegin = ti->ti_begin;
	curend = ti->ti_end;

	tib = ti->ti_firstblock;

	while (tib != NULL) {

		/* See if there still is data. */
		if (curbegin == curend)
			break;

		/*
		 * The end address should be the lowest of these three:
		 * - The write pointer
		 * - The blocksize - we can't read beyond the block
		 */
		cbegin = curbegin;
		cend = MIN(curend, TTYINQ_DATASIZE);
		clen = cend - cbegin;

		/* Copy the data out of the buffers. */
		sbuf_bcat(sb, tib->tib_data + cbegin, clen);
		copied += clen;

		if (cend == ti->ti_end) {
			/* Read the complete queue. */
			curbegin = 0;
			curend = 0;
		} else if (cend == TTYINQ_DATASIZE) {
			/* Read the block until the end. */
			curbegin = 0;
			curend -= TTYINQ_DATASIZE;

			tib = tib->tib_next;
		} else {
			/* Read the block partially. */
			curbegin += clen;
		}
	}

	sbuf_finish(sb);

	/* Write the data to the main sbuf. */
	data = sbuf_data(sb);
	datasize = sbuf_len(sb);

	error = sbuf_bcat(ptssb, &datasize, sizeof(datasize));
	if (error != 0)
		goto out;

	error = sbuf_bcat(ptssb, data, datasize);
	if (error != 0)
		goto out;

out:
	sbuf_delete(sb);

	return (error);
}

/*
 * Variant of ttyoutq_read() that nondestructively reads all data from
 * the output queue into an sbuf.
 */
static int
slsckpt_ttyoutq_read(struct ttyoutq *to, struct sbuf *ptssb)
{
	size_t curbegin, curend;
	struct ttyoutq_block *tob;
	size_t cbegin, cend, clen;
	size_t copied = 0;
	struct sbuf *sb;
	size_t datasize;
	int error = 0;
	void *data;

	sb = sbuf_new_auto();

	/* We don't want to modify the queue itself, so we use our own indices.
	 */
	curbegin = to->to_begin;
	curend = to->to_end;

	tob = to->to_firstblock;

	while (tob != NULL) {

		/* See if there still is data. */
		if (curbegin == curend)
			break;

		/*
		 * The end address should be the lowest of these three:
		 * - The write pointer
		 * - The blocksize - we can't read beyond the block
		 */
		cbegin = curbegin;
		cend = MIN(curend, TTYOUTQ_DATASIZE);
		clen = cend - cbegin;

		/* Copy the data out of the buffers. */
		sbuf_bcat(sb, tob->tob_data + cbegin, clen);
		copied += clen;

		if (cend == to->to_end) {
			/* Read the complete queue. */
			curbegin = 0;
			curend = 0;
		} else if (cend == TTYOUTQ_DATASIZE) {
			/* Read the block until the end. */
			curbegin = 0;
			curend -= TTYOUTQ_DATASIZE;

			tob = tob->tob_next;
		} else {
			/* Read the block partially. */
			curbegin += clen;
		}
	}

	sbuf_finish(sb);

	/* Write the data to the main sbuf. */
	data = sbuf_data(sb);
	datasize = sbuf_len(sb);

	error = sbuf_bcat(ptssb, &datasize, sizeof(datasize));
	if (error != 0)
		goto out;

	error = sbuf_bcat(ptssb, data, datasize);
	if (error != 0)
		goto out;

out:
	sbuf_delete(sb);

	return (error);
}

static int
slspts_checkpoint(
    struct file *fp, struct sbuf *sb, struct slsckpt_data *sckpt_data)
{
	struct tty *tty = (struct tty *)fp->f_data;
	struct slspts slspts;
	int error;

	/* Get the data from the PTY. */
	slspts.magic = SLSPTS_ID;
	slspts.slsid = (uint64_t)tty;
	/* This is the master side of the tty. */
	slspts.ismaster = 1;
	/* We use the cdev as the peer's ID. */
	slspts.peerid = (uint64_t)tty->t_dev;
	slspts.drainwait = tty->t_drainwait;
	slspts.termios = tty->t_termios;
	slspts.winsize = tty->t_winsize;
	slspts.writepos = tty->t_writepos;
	slspts.termios_init_in = tty->t_termios_init_in;
	slspts.termios_init_out = tty->t_termios_init_out;
	slspts.termios_lock_in = tty->t_termios_lock_in;
	slspts.termios_lock_out = tty->t_termios_lock_out;
	slspts.flags = tty->t_flags;
	KASSERT(
	    ((tty->t_flags & TF_BUSY) == 0), ("PTS checkpointed while busy"));

	/* Add it to the record. */
	error = sbuf_bcat(sb, (void *)&slspts, sizeof(slspts));
	if (error != 0)
		return (error);

	/* Get the data. */
	error = slsckpt_ttyinq_read(&tty->t_inq, sb);
	if (error != 0)
		return (error);

	error = slsckpt_ttyoutq_read(&tty->t_outq, sb);
	if (error != 0)
		return (error);

	return (0);
}

int
slspts_checkpoint_vnode(struct vnode *vp, struct sbuf *sb)
{
	struct slspts slspts;
	int error;

	/* Get the data from the PTY. */
	slspts.magic = SLSPTS_ID;
	slspts.slsid = (uint64_t)vp->v_rdev;
	slspts.ismaster = 0;
	/* Our peer has the tty's pointer as its ID. */
	slspts.peerid = (uint64_t)vp->v_rdev->si_drv1;

	/* We don't need anything else, it's in the master's record. */

	/* Add it to the record. */
	error = sbuf_bcat(sb, (void *)&slspts, sizeof(slspts));
	if (error != 0)
		return (error);

	return (0);
}

static int
slspts_restore_slv(struct tty *tty, struct file **fpp)
{
	struct thread *td = curthread;
	struct file *fp;
	char *path;
	int error;
	int fd;

	/* Get the name of the slave side. */
	path = malloc(PATH_MAX, M_SLSMM, M_WAITOK);
	strlcpy(path, DEVFS_ROOT, sizeof(DEVFS_ROOT));
	strlcat(path, devtoname(tty->t_dev), PATH_MAX);

	error = kern_openat(td, AT_FDCWD, path, UIO_SYSSPACE, O_RDWR, S_IRWXU);
	free(path, M_SLSMM);
	if (error != 0)
		return (error);

	fd = td->td_retval[0];
	error = slsfile_extractfp(fd, &fp);
	if (error != 0) {
		slsfile_attemptclose(fd);
		return (error);
	}

	*fpp = fp;

	return (0);
}

static int
slspts_restore_ttyq(struct slspts *slspts, struct tty *tty)
{
	size_t written;

	/* Fill back in the tty input and output queues. */
	if (slspts->inq != NULL) {
		written = ttyinq_write(
		    &tty->t_inq, slspts->inq, slspts->inqlen, 0);
		if (written != slspts->inqlen)
			return (EINVAL);
	}

	if (slspts->outq != NULL) {
		written = ttyoutq_write(
		    &tty->t_outq, slspts->outq, slspts->outqlen);
		if (written != slspts->outqlen)
			return (EINVAL);
	}

	return (0);
}

/*
 * Modified version of sys_posix_openpt(). Restores
 * both the master and the slave side of the pts.
 */
static int
slspts_restore(void *slsbacker, struct slsfile *finfo,
    struct slsrest_data *restdata, struct file **fpp)
{
	struct slspts *slspts = (struct slspts *)slsbacker;
	struct file *masterfp, *slavefp;
	struct file *localfp, *peerfp;
	struct tty *tty;
	uintptr_t peer;
	uint64_t slsid;
	int error;

	/* If the peer has been already restored we don't need to do anything.
	 */
	slsid = slspts->slsid;
	if (slskv_find(restdata->fptable, slsid, &peer) == 0) {
		*fpp = NULL;
		return (0);
	}

	/*
	 * We don't really want the fd, but all the other file
	 * type restore routines create one, so we do too and
	 * get it fixed up back in the common path.
	 */
	error = falloc_noinstall(curthread, &masterfp);
	if (error != 0)
		return (error);

	/*
	 * XXX Actually check whether we want NOCTTY, or else manually
	 * manually set the controlling terminal of a process elsewhere.
	 */
	error = pts_alloc(FREAD | FWRITE | O_NOCTTY, curthread, masterfp);
	if (error != 0) {
		fdrop(masterfp, curthread);
		return (error);
	}

	tty = masterfp->f_data;
	/* XXX See if there are flags we can (slstty->flags to t_flags).  */

	KASSERT(tty->t_dev != NULL, ("device is null"));
	KASSERT(tty->t_dev->si_devsw != NULL, ("cdevsw is null"));

	/*
	 * XXX Check whether we need the rest of the shell's parameters in any
	 * case, and how to properly restore them if we do.
	 */

	/* As in the case of pipes, we add the peer to the table ourselves. */
	error = slspts_restore_slv(tty, &slavefp);
	if (error != 0) {
		return (error);
	}

	/*
	 * We always save the peer in this function, regardless of whether it's
	 * master. That's because the caller always looks at the slsid field,
	 * and combines it with the fd that we return to it.
	 */
	if (slspts->ismaster != 0) {
		localfp = masterfp;
		peerfp = slavefp;
	} else {
		localfp = slavefp;
		peerfp = masterfp;
	}

	/*
	 * Externalize here so that we clean up in the caller if the
	 * rest of the function fails.
	 */
	*fpp = localfp;

	error = slskv_add(restdata->fptable, slspts->peerid, (uintptr_t)peerfp);
	if (error != 0) {
		fdrop(peerfp, curthread);
		return (error);
	}

	error = slspts_restore_ttyq(slspts, tty);

	return (error);
}

static int
slspts_slsid(struct file *fp, uint64_t *slsidp)
{
	/*
	 * Pseudoterminals come in pairs, but one side is an
	 * actual pts file, the other is a device vnode.
	 * We handle the device vnode separately.
	 */
	*slsidp = (uint64_t)fp->f_data;

	return (0);
}

static bool
slspts_supported(struct file *fp)
{
	return (true);
}

struct slsfile_ops slspts_ops = {
	.slsfile_supported = slspts_supported,
	.slsfile_slsid = slspts_slsid,
	.slsfile_checkpoint = slspts_checkpoint,
	.slsfile_restore = slspts_restore,
};
