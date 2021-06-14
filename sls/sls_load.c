#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <machine/param.h>
#include <machine/reg.h>

#include <slos.h>
#include <slos_inode.h>

#include "debug.h"
#include "sls_data.h"
#include "sls_ioctl.h"
#include "sls_load.h"
#include "sls_vmspace.h"
#include "sysv_internal.h"

/*
 * Read an info struct from the buffer, if there is enough data,
 * and adjust bufp and bufisizep to account for the read data.
 */
int
sls_info(void *info, size_t infosize, char **bufp, size_t *bufsizep)
{
	if (*bufsizep < infosize)
		return (EINVAL);

	memcpy(info, *bufp, infosize);

	*bufp += infosize;
	*bufsizep -= infosize;

	return (0);
}

int
slsload_thread(struct slsthread *slsthread, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsthread, sizeof(*slsthread), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (slsthread->magic != SLSTHREAD_ID) {
		DEBUG("magic mismatch\n");
		return (EINVAL);
	}

	return (0);
}

/* Functions that load parts of the state. */
int
slsload_proc(struct slsproc *slsproc, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsproc, sizeof(*slsproc), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (slsproc->magic != SLSPROC_ID) {
		SLS_DBG(
		    "magic mismatch, %lx vs %x\n", slsproc->magic, SLSPROC_ID);
		return (EINVAL);
	}

	return (0);
}

static int
slsload_kqueue(slsset **slsknsp, char **bufp, size_t *bufsizep)
{
	struct slskqueue *kqinfo = NULL;
	struct slsknote *slsknote;
	slsset *slskns = NULL;
	int error;

	kqinfo = malloc(sizeof(*kqinfo), M_SLSMM, M_WAITOK);

	/* Read in the kqueue itself. */
	error = sls_info(kqinfo, sizeof(*kqinfo), bufp, bufsizep);
	if (error != 0)
		goto error;

	if (kqinfo->magic != SLSKQUEUE_ID) {
		SLS_DBG(
		    "magic mismatch, %lu vs %d\n", kqinfo->magic, SLSKQUEUE_ID);
		error = EINVAL;
		goto error;
	}

	/* The rest of the buffer is the list of kevents. */
	error = slsset_create(&slskns);
	if (error != 0)
		goto error;

	/* Save the metadata in the set's private data field. */
	slskns->data = kqinfo;

	/* Go through the record, splitting it into elements. */
	while (*bufsizep > 0) {
		slsknote = malloc(sizeof(*slsknote), M_SLSMM, M_WAITOK);

		error = sls_info(slsknote, sizeof(*slsknote), bufp, bufsizep);
		if (error != 0)
			goto error;

		error = slsset_add(slskns, (uint64_t)slsknote);
		if (error != 0)
			goto error;
	}

	/* Associate the kqueue metadata with the set of kevents. */
	slskns->data = kqinfo;

	/* Export to the caller. */
	*slsknsp = slskns;

	return (0);

error:
	/* Free each kevent entry separately. */
	if (slskns != NULL) {
		KVSET_FOREACH_POP(slskns, slsknote)
		free(slsknote, M_SLSMM);

		slsset_destroy(slskns);
	}

	free(kqinfo, M_SLSMM);
	return (error);
}

static int
slsload_pipe(struct slspipe *slspipe, char **bufp, size_t *bufsizep)
{
	int error;

	/* Read in the kqueue itself. */
	error = sls_info(slspipe, sizeof(*slspipe), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (slspipe->magic != SLSPIPE_ID) {
		SLS_DBG(
		    "magic mismatch, %lx vs %x\n", slspipe->magic, SLSPIPE_ID);
		return (EINVAL);
	}

	/* Get the pipe's data. */
	slspipe->data = malloc(slspipe->pipebuf.cnt, M_SLSMM, M_WAITOK);
	error = sls_info(slspipe->data, slspipe->pipebuf.cnt, bufp, bufsizep);
	if (error != 0) {
		free(slspipe->data, M_SLSMM);
		return error;
	}

	return (0);
}

/* Get an array that is prefixed by its size. */
static int
slsload_sizedarray(void **datap, size_t *sizep, char **bufp, size_t *bufsizep)
{
	size_t size;
	void *data;
	int error;

	/* Get the size of the data. */
	error = sls_info(&size, sizeof(size), bufp, bufsizep);
	if (error != 0)
		return (error);

	/* Get the data itself. */
	data = malloc(size, M_SLSMM, M_WAITOK);
	error = sls_info(data, size, bufp, bufsizep);
	if (error != 0) {
		free(data, M_SLSMM);
		return (error);
	}

	/* Externalize the data. */
	*sizep = size;
	*datap = data;

	return (0);
}

static int
slsload_pts(struct slspts *slspts, char **bufp, size_t *bufsizep)
{
	int error;

	/* Read in the kqueue itself. */
	error = sls_info(slspts, sizeof(*slspts), bufp, bufsizep);
	if (error != 0)
		return (error);
	slspts->inq = NULL;
	slspts->outq = NULL;

	if (slspts->magic != SLSPTS_ID) {
		SLS_DBG(
		    "magic mismatch, %lx vs %x\n", slspts->magic, SLSPTS_ID);
		return (EINVAL);
	}

	/* No data if we're not the master. */
	if (slspts->ismaster == 0)
		return (0);

	error = slsload_sizedarray(
	    &slspts->inq, &slspts->inqlen, bufp, bufsizep);
	if (error != 0)
		return (error);

	error = slsload_sizedarray(
	    &slspts->outq, &slspts->outqlen, bufp, bufsizep);
	if (error != 0) {
		/* The first allocation succeeded, undo it. */
		free(slspts->inq, M_SLSMM);
		return (error);
	}

	return (0);
}

int
slsload_vnode(struct slsvnode *slsvnode, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsvnode, sizeof(*slsvnode), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (slsvnode->magic != SLSVNODE_ID) {
		SLS_DBG("magic mismatch, %lx vs %x\n", slsvnode->magic,
		    SLSVNODE_ID);
		return (EINVAL);
	}

	return (0);
}

static int
slsload_socket(struct slssock *slssock, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slssock, sizeof(*slssock), bufp, bufsizep);
	if (error != 0)
		return (error);

	return (0);
}

static int
slsload_posixshm(struct slsposixshm *slsposixshm, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsposixshm, sizeof(*slsposixshm), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (slsposixshm->magic != SLSPOSIXSHM_ID) {
		SLS_DBG("magic mismatch, %lx vs %x\n", slsposixshm->magic,
		    SLSPOSIXSHM_ID);
		return (EINVAL);
	}

	return (0);
}

int
slsload_file(
    struct slsfile *slsfile, void **data, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsfile, sizeof(*slsfile), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (slsfile->magic != SLSFILE_ID) {
		SLS_DBG("magic mismatch (%lx, expected %x)\n", slsfile->magic,
		    SLSFILE_ID);
		return (EINVAL);
	}

	switch (slsfile->type) {
	case DTYPE_VNODE:
	case DTYPE_FIFO:
		/* Nothing to do, the vnode is already restored. */
		break;

	case DTYPE_KQUEUE:
		/*
		 * The data for the kqueue the kqueue metadata and kevents.
		 */
		error = slsload_kqueue((slsset **)data, bufp, bufsizep);
		break;

	case DTYPE_PIPE:
		*data = malloc(sizeof(struct slspipe), M_SLSMM, M_WAITOK);
		error = slsload_pipe((struct slspipe *)*data, bufp, bufsizep);
		break;

	case DTYPE_SOCKET:
		*data = malloc(sizeof(struct slssock), M_SLSMM, M_WAITOK);
		error = slsload_socket((struct slssock *)*data, bufp, bufsizep);
		break;

	case DTYPE_PTS:
		*data = malloc(sizeof(struct slspts), M_SLSMM, M_WAITOK);
		error = slsload_pts((struct slspts *)*data, bufp, bufsizep);
		break;

	case DTYPE_SHM:
		*data = malloc(sizeof(struct slsposixshm), M_SLSMM, M_WAITOK);
		error = slsload_posixshm(
		    (struct slsposixshm *)*data, bufp, bufsizep);
		break;

	default:
		panic("unhandled file type");
	}

	if (error != 0)
		return (error);

	return (0);
}

int
slsload_filedesc(struct slsfiledesc **filedescp, char **bufp, size_t *bufsizep)
{
	struct slsfiledesc *filedesc;
	uint64_t fd_size;
	int error;

	error = sls_info(&fd_size, sizeof(fd_size), bufp, bufsizep);
	if (error != 0)
		return (0);

	filedesc = malloc(fd_size, M_SLSMM, M_WAITOK);

	/* General file descriptor */
	error = sls_info(filedesc, fd_size, bufp, bufsizep);
	if (error != 0) {
		free(filedesc, M_SLSMM);
		return (0);
	}

	if (filedesc->magic != SLSFILEDESC_ID) {
		SLS_DBG("magic mismatch, %lx vs %x\n", filedesc->magic,
		    SLSFILEDESC_ID);
		free(filedesc, M_SLSMM);
		return (EINVAL);
	}

	*filedescp = filedesc;

	return (0);
}

int
slsload_vmentry(struct slsvmentry *entry, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(entry, sizeof(*entry), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (entry->magic != SLSVMENTRY_ID) {
		SLS_DBG("magic mismatch");
		return (EINVAL);
	}

	return (0);
}

int
slsload_vmspace(struct slsvmspace *vm, struct shmmap_state **shmstatep,
    char **bufp, size_t *bufsizep)
{
	struct shmmap_state *shmstate = NULL;
	size_t shmsize;
	int error;

	*shmstatep = NULL;

	error = sls_info(vm, sizeof(*vm), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (vm->magic != SLSVMSPACE_ID) {
		SLS_DBG("magic mismatch\n");
		return (EINVAL);
	}

	if (vm->has_shm != 0) {
		shmsize = sizeof(*shmstate) * shminfo.shmseg;
		shmstate = malloc(shmsize, M_SHM, M_WAITOK);

		error = sls_info(shmstate, shmsize, bufp, bufsizep);
		if (error != 0) {
			free(shmstate, M_SHM);
			return (error);
		}
	}

	*shmstatep = shmstate;

	return (0);
}

int
slsload_sysvshm(struct slssysvshm *shm, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(shm, sizeof(*shm), bufp, bufsizep);
	if (error != 0)
		return (error);

	if (shm->magic != SLSSYSVSHM_ID) {
		SLS_DBG("magic mismatch\n");
		return (EINVAL);
	}

	return (0);
}

int
slsload_sockbuf(struct mbuf **mp, uint64_t *sbid, char **bufp, size_t *bufsizep)
{
	struct mbuf *lasthead, *lastrec;
	struct mbuf *m, *headm;
	struct slsmbuf slsmbuf;
	int newrec;
	int error;

	newrec = 0;
	m = headm = NULL;
	lasthead = lastrec = NULL;
	while (*bufsizep > 0) {
		error = sls_info(&slsmbuf, sizeof(slsmbuf), bufp, bufsizep);
		if (error != 0)
			return (error);

		if (slsmbuf.magic != SLSMBUF_ID) {
			SLS_DBG("magic mismatch\n");
			return (EINVAL);
		}

		/* Allocate the packet, checking if it's a header. */
		if (slsmbuf.flags & M_PKTHDR)
			MGETHDR(m, M_WAITOK, slsmbuf.type);
		else
			MGET(m, M_WAITOK, slsmbuf.type);

		/* If the buffer was a cluster, attach a new cluster to it. */
		if (slsmbuf.flags & M_EXT)
			MCLGET(m, M_WAITOK);

		/* Copy the data into the mbuf. */
		error = sls_info(mtod(m, void *), slsmbuf.len, bufp, bufsizep);
		if (error != 0) {
			while (headm != NULL) {
				m = headm;
				headm = headm->m_nextpkt;
				m_free(m);
			}

			return (error);
		}

		/* XXX Restore CMSGs, if they exist. */

		if (lasthead == NULL) {
			/* If there is no mbuf head, we're the first one. */
			lasthead = m;
			lastrec = m;
			headm = m;
		} else if (newrec != 0) {
			/* Else check if we're a brand new message. */
			lasthead->m_nextpkt = m;
			lasthead = m;
			newrec = 0;
		} else {
			/* Otherwise just append to the end. */
			lastrec->m_next = m;
			lastrec = m;
		}

		/* New packet starting from the next buffer. */
		if (slsmbuf.flags & M_EOR)
			newrec = 1;
	}

	/* Export the results to the caller. */
	*sbid = slsmbuf.slsid;
	*mp = headm;

	return (0);
}
