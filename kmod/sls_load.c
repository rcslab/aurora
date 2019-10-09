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

#include "sls.h"
#include "slsmm.h"
#include "sls_data.h"
#include "sls_ioctl.h"
#include "sls_load.h"
#include "sls_mem.h"

#include <slos.h>
#include "../slos/slos_inode.h"
#include "../slos/slos_io.h"
#include "../slos/slos_record.h"

/* 
 * Read an info struct from the buffer, if there is enough data,
 * and adjust bufp and bufisizep to account for the read data.
 */
static int
sls_info(void *info, size_t infosize, char **bufp, size_t *bufsizep)
{
	if (*bufsizep < infosize)
	    return EINVAL;

	memcpy(info, *bufp, infosize);

	*bufp += infosize;
	*bufsizep -= infosize;

	return 0;
}

int
sls_load_thread(struct thread_info *thread_info, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(thread_info, sizeof(*thread_info), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (thread_info->magic != SLS_THREAD_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	return 0;
}

/* Functions that load parts of the state. */
int 
sls_load_proc(struct proc_info *proc_info, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(proc_info, sizeof(*proc_info), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (proc_info->magic != SLS_PROC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch, %lu vs %d\n", proc_info->magic, SLS_PROC_INFO_MAGIC);
	    return EINVAL;
	}

	return 0;
}

extern struct kevent_info sls_kevents[1024]; 

static int
sls_load_kqueue(struct kqueue_info *kqinfo, char **bufp, size_t *bufsizep)
{
	int error; 

	/* Read in the kqueue itself. */
	error = sls_info(kqinfo, sizeof(*kqinfo), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (kqinfo->magic != SLS_KQUEUE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch, %lu vs %d\n", kqinfo->magic, SLS_KQUEUE_INFO_MAGIC);
	    return EINVAL;
	}

	/* Read in the kevents for this kqueue. */
	error = sls_info(sls_kevents, kqinfo->numevents * sizeof(struct kevent_info), bufp, bufsizep);
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_load_pipe(struct pipe_info *ppinfo, char **bufp, size_t *bufsizep)
{
	int error; 

	/* Read in the kqueue itself. */
	error = sls_info(ppinfo, sizeof(*ppinfo), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (ppinfo->magic != SLS_PIPE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch, %lx vs %x\n", ppinfo->magic, SLS_PIPE_INFO_MAGIC);
	    return EINVAL;
	}

	return 0;
}

static int
sls_load_vnode(struct sbuf **path, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_load_path(path, bufp, bufsizep);
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_load_socket(struct sock_info *sock_info, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(sock_info, sizeof(*sock_info), bufp, bufsizep);
	if (error != 0)
	    return error;

	return 0;
}

int
sls_load_file(struct file_info *file_info, void **data, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(file_info, sizeof(*file_info), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (file_info->magic == SLS_FILES_END)
	    return 0;

	if (file_info->magic != SLS_FILE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch");
	    return EINVAL;
	}

	switch (file_info->type) {
	case DTYPE_VNODE:
	    error = sls_load_vnode((struct sbuf **) data, bufp, bufsizep);
	    break;

	case DTYPE_KQUEUE:
	    *data = malloc(sizeof(struct kqueue_info), M_SLSMM, M_WAITOK);
	    error = sls_load_kqueue((struct kqueue_info *) *data, bufp, bufsizep);
	    break;

	case DTYPE_PIPE:
	    *data = malloc(sizeof(struct pipe_info), M_SLSMM, M_WAITOK);
	    error = sls_load_pipe((struct pipe_info *) *data, bufp, bufsizep);
	    break;

	case DTYPE_SOCKET:
	    *data = malloc(sizeof(struct sock_info), M_SLSMM, M_WAITOK);
	    error = sls_load_socket((struct sock_info *) *data, bufp, bufsizep);
	    break;

	default:
	    panic("unhandled file type");
	}
	
	if (error != 0)
	    return error;


	return 0;
}

int
sls_load_filedesc(struct filedesc_info *filedesc, char **bufp, size_t *bufsizep)
{
	int error;

	/* General file descriptor */
	error = sls_info(filedesc, sizeof(*filedesc), bufp, bufsizep);
	if (error != 0)
	    return 0;

	if (filedesc->magic != SLS_FILEDESC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	/* Current and root directories */
	error = sls_load_path(&filedesc->cdir, bufp, bufsizep);
	if (error != 0)
	    return error;

	error = sls_load_path(&filedesc->rdir, bufp, bufsizep);
	if (error != 0) {
	    sbuf_delete(filedesc->cdir);
	    return error;
	}

	return 0;
}

int
sls_load_vmobject(struct vm_object_info *obj, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(obj, sizeof(*obj), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (obj->magic == SLS_OBJECTS_END)
	    return 0;

	if (obj->magic != SLS_OBJECT_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	if (obj->type == OBJT_VNODE) {
	    error = sls_load_path(&obj->path, bufp, bufsizep);
	    if (error != 0)
		return error;
	}

	return 0;
}

int
sls_load_vmentry(struct vm_map_entry_info *entry, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(entry, sizeof(*entry), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (entry->magic != SLS_ENTRY_INFO_MAGIC) {
	    SLS_DBG("magic mismatch");
	    return EINVAL;
	}

	return 0;
}

int 
sls_load_memory(struct memckpt_info *memory, char **bufp, size_t *bufsizep)
{
	int error;

	error = sls_info(memory, sizeof(*memory), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (memory->vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	return 0;
}

int
sls_load_path(struct sbuf **sbp, char **bufp, size_t *bufsizep) 
{
	int error;
	size_t sblen;
	char *path = NULL;
	int magic;
	struct sbuf *sb = NULL;

	error = sls_info(&magic, sizeof(magic), bufp, bufsizep);
	if (error != 0)
	    return error;

	if (magic != SLS_STRING_MAGIC)
	    return EINVAL;

	error = sls_info(&sblen, sizeof(sblen), bufp, bufsizep);
	if (error != 0)
	    return error;

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

	return 0;

error:

	if (sb != NULL)
	    sbuf_delete(sb);

	free(path, M_SLSMM);
	*sbp = NULL;
	return error;

}
