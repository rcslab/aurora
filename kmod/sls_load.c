#include <sys/types.h>

#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>

#include <machine/param.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "sls_data.h"
#include "sls_ioctl.h"
#include "sls_load.h"
#include "sls_mm.h"
#include "sls_vmspace.h"

#include <slos.h>
#include <slos_record.h>
#include <slos_io.h>

/* 
 * Read an info struct from the buffer, if there is enough data,
 * and adjust bufp and bufisizep to account for the read data.
 */
static int
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
	    SLS_DBG("magic mismatch\n");
	    return (EINVAL);
	}

	return (0);
}

/* Functions that load parts of the state. */
int 
slsload_proc(struct slsproc *slsproc, struct sbuf **namep, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsproc, sizeof(*slsproc), bufp, bufsizep);
	if (error != 0)
	    return (error);

	if (slsproc->magic != SLSPROC_ID) {
	    SLS_DBG("magic mismatch, %lu vs %d\n", slsproc->magic, SLSPROC_ID);
	    return (EINVAL);
	}

	error = slsload_path(namep, bufp, bufsizep);
	if (error != 0)
	    return (error);
	
	return (0);
}

static int
slsload_kqueue(slsset **kevsetp, char **bufp, size_t *bufsizep)
{
	struct slskqueue *kqinfo = NULL;
	struct slskevent *kevinfo;
	slsset *kevset = NULL;
	int error; 

	kqinfo = malloc(sizeof(*kqinfo), M_SLSMM, M_WAITOK);

	/* Read in the kqueue itself. */
	error = sls_info(kqinfo, sizeof(*kqinfo), bufp, bufsizep);
	if (error != 0)
	    return (error);

	if (kqinfo->magic != SLSKQUEUE_ID) {
	    SLS_DBG("magic mismatch, %lu vs %d\n", kqinfo->magic, SLSKQUEUE_ID);
	    error = EINVAL;
	    goto error;
	}

	/* The rest of the buffer is the list of kevents. */
	error = slsset_create(&kevset);
	if (error != 0) {
	    error = EINVAL;
	    goto error;
	}

	/* Save the metadata in the set's private data field. */
	kevset->data = kqinfo;

	/* Go throught the record, splitting it into elements. */
	while (*bufsizep > 0) {
	    kevinfo = malloc(sizeof(*kevinfo), M_SLSMM, M_WAITOK);

	    error = sls_info(kevinfo, sizeof(*kevinfo), bufp, bufsizep);
	    if (error != 0)
		goto error;

	    error = slsset_add(kevset, (uint64_t) kevinfo);
	    if (error != 0)
		goto error;
	}

	/* Associate the kqueue metadata with the set of kevents. */
	kevset->data = kqinfo;

	/* Export to the caller. */
	*kevsetp = kevset;

	return (0);

error:
	/* Free each kevent entry separately. */
	if (kevset != NULL) {
	    while (slsset_pop(kevset, (uint64_t *) &kevinfo) != 0)
		free(kevinfo, M_SLSMM);

	    slsset_destroy(kevset);
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
	    SLS_DBG("magic mismatch, %lx vs %x\n", slspipe->magic, SLSPIPE_ID);
	    return (EINVAL);
	}

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

	if (slspts->magic != SLSPTS_ID) {
	    SLS_DBG("magic mismatch, %lx vs %x\n", slspts->magic, SLSPTS_ID);
	    return (EINVAL);
	}

	return (0);
}

static int
slsload_vnode(struct sbuf **path, char **bufp, size_t *bufsizep)
{
	int error;

	error = slsload_path(path, bufp, bufsizep);
	if (error != 0)
	    return (error);

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

int
slsload_file(struct slsfile *slsfile, void **data, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(slsfile, sizeof(*slsfile), bufp, bufsizep);
	if (error != 0)
	    return (error);

	if (slsfile->magic != SLSFILE_ID) {
	    SLS_DBG("magic mismatch (%lx, expected %x", slsfile->magic, SLSFILE_ID);
	    return (EINVAL);
	}

	switch (slsfile->type) {
	case DTYPE_VNODE:
	    error = slsload_vnode((struct sbuf **) data, bufp, bufsizep);
	    break;

	case DTYPE_KQUEUE:
	    /* 
	     * The data for the kqueue is both 
	     * the core metadata and a set of kevents.
	     */
	    error = slsload_kqueue((slsset **) data, bufp, bufsizep);
	    break;

	case DTYPE_PIPE:
	    *data = malloc(sizeof(struct slspipe), M_SLSMM, M_WAITOK);
	    error = slsload_pipe((struct slspipe *) *data, bufp, bufsizep);
	    break;

	case DTYPE_SOCKET:
	    *data = malloc(sizeof(struct slssock), M_SLSMM, M_WAITOK);
	    error = slsload_socket((struct slssock *) *data, bufp, bufsizep);
	    break;

	case DTYPE_PTS:
	    *data = malloc(sizeof(struct slspts), M_SLSMM, M_WAITOK);
	    error = slsload_pts((struct slspts *) *data, bufp, bufsizep);
	    break;

	default:
	    panic("unhandled file type");
	}
	
	if (error != 0)
	    return (error);


	return (0);
}

int
slsload_filedesc(struct slsfiledesc *filedesc, char **bufp, size_t *bufsizep, struct slskv_table **fdtable)
{
	int error;

	/* General file descriptor */
	error = sls_info(filedesc, sizeof(*filedesc), bufp, bufsizep);
	if (error != 0)
	    return (0);

	if (filedesc->magic != SLSFILEDESC_ID) {
	    SLS_DBG("magic mismatch, %lx vs %x\n", filedesc->magic, SLSFILEDESC_ID);
	    return (EINVAL);
	}

	/* Current and root directories */
	error = slsload_path(&filedesc->cdir, bufp, bufsizep);
	if (error != 0)
	    return (error);

	error = slsload_path(&filedesc->rdir, bufp, bufsizep);
	if (error != 0) {
	    sbuf_delete(filedesc->cdir);
	    return (error);
	}

	error = slskv_deserial(*bufp, *bufsizep, fdtable);
	if (error != 0)
	    return (error);

	return (0);
}

int
slsload_vmobject(struct slsvmobject *obj, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(obj, sizeof(*obj), bufp, bufsizep);
	if (error != 0)
	    return (error);

	if (obj->magic != SLSVMOBJECT_ID) {
	    SLS_DBG("magic mismatch\n");
	    return (EINVAL);
	}

	if (obj->type == OBJT_VNODE) {
	    error = slsload_path(&obj->path, bufp, bufsizep);
	    if (error != 0)
		return (error);
	}

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
slsload_vmspace(struct slsvmspace *vm, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(vm, sizeof(*vm), bufp, bufsizep);
	if (error != 0)
	    return (error);

	if (vm->magic != SLSVMSPACE_ID) {
	    SLS_DBG("magic mismatch\n");
	    return (EINVAL);
	}

	return (0);
}

int
slsload_path(struct sbuf **sbp, char **bufp, size_t *bufsizep) 
{
	int error;
	size_t sblen;
	char *path = NULL;
	int magic;
	struct sbuf *sb = NULL;

	error = sls_info(&magic, sizeof(magic), bufp, bufsizep);
	if (error != 0)
	    return (error);

	if (magic != SLSSTRING_ID)
	    return (EINVAL);

	error = sls_info(&sblen, sizeof(sblen), bufp, bufsizep);
	if (error != 0)
	    return (error);

	/* First copy the data into a temporary raw buffer */
	path = malloc(sblen + 1, M_SLSMM, M_WAITOK);
	error = sls_info(path, sblen, bufp, bufsizep);
	if (error != 0)
	    goto error;
	path[sblen++] = '\0';

	sb = sbuf_new_auto();
	if (sb == NULL)
	    goto error;

	/* Then move it over to the sbuf */
	error = sbuf_bcpy(sb, path, sblen);
	if (error != 0)
	    goto error;

	error = sbuf_finish(sb);
	if (error != 0)
	    goto error;

	*sbp = sb;
	free(path, M_SLSMM);

	return (0);

error:

	if (sb != NULL)
	    sbuf_delete(sb);

	free(path, M_SLSMM);
	*sbp = NULL;
	return (error);

}
