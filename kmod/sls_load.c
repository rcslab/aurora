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
#include "sls_channel.h"
#include "sls_data.h"
#include "sls_ioctl.h"
#include "sls_load.h"
#include "sls_mem.h"

#include <slos.h>
#include "../slos/slos_inode.h"
#include "../slos/slos_io.h"
#include "../slos/slos_record.h"

/* XXX Will become a sysctl value */
size_t sls_contig_limit = 2 * 1024 * 1024;

#define SLS_MSG_END ULONG_MAX 

static int 
htable_init(struct sls_pagetable *ptable)
{
	ptable->pages = hashinit(HASH_MAX, M_SLSMM, &ptable->hashmask);
	if (ptable->pages == NULL)
		return ENOMEM;

	return 0;
}

static void
htable_fini(struct sls_pagetable *ptable)
{
	int i;
	struct dump_page *cur_page;
	struct page_list *cur_bucket;

	for (i = 0; i <= ptable->hashmask; i++) {
		cur_bucket = &ptable->pages[i];
		while (!LIST_EMPTY(cur_bucket)) {
			cur_page = LIST_FIRST(cur_bucket);
			LIST_REMOVE(cur_page, next);
			free(cur_page->data, M_SLSMM);
			free(cur_page, M_SLSMM);
		}
	}

	hashdestroy(ptable->pages, M_SLSMM, ptable->hashmask);
}


static void
addpage_noreplace(struct sls_pagetable *ptable, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;

	vaddr = new_entry->vaddr;
	page_bucket = &ptable->pages[vaddr & ptable->hashmask];

	LIST_FOREACH(page_entry, page_bucket, next) {
	    if(page_entry->vaddr == new_entry->vaddr) {
		free(new_entry->data, M_SLSMM);
		free(new_entry, M_SLSMM);
		return;
	    }
	}

	LIST_INSERT_HEAD(page_bucket, new_entry, next);
}

int
sls_load_thread(struct thread_info *thread_info, struct sls_channel *chan)
{
	int error;

	error = sls_read(thread_info, sizeof(*thread_info), SLOSREC_PROC, chan);
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
sls_load_proc(struct proc_info *proc_info, struct sls_channel *chan)
{
	int error;

	error = sls_read(proc_info, sizeof(*proc_info), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	if (proc_info->magic != SLS_PROC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch, %d vs %d\n", proc_info->magic, SLS_PROC_INFO_MAGIC);
	    return EINVAL;
	}

	return 0;
}

int
sls_load_file(struct file_info *file_info, struct sls_channel *chan)
{
	int error;

	error = sls_read(file_info, sizeof(*file_info), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	if (file_info->magic == SLS_FILES_END)
	    return 0;

	if (file_info->magic != SLS_FILE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch");
	    return EINVAL;
	}

	error = sls_load_path(&file_info->path, chan);
	if (error != 0)
	    return error;

	return 0;
}

int
sls_load_filedesc(struct filedesc_info *filedesc, struct sls_channel *chan)
{
	int error;

	/* General file descriptor */
	error = sls_read(filedesc, sizeof(*filedesc), SLOSREC_PROC, chan);
	if (error != 0)
	    return 0;

	if (filedesc->magic != SLS_FILEDESC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	/* Current and root directories */
	error = sls_load_path(&filedesc->cdir, chan);
	if (error != 0)
	    return error;

	error = sls_load_path(&filedesc->rdir, chan);
	if (error != 0) {
	    sbuf_delete(filedesc->cdir);
	    return error;
	}

	return 0;
}

int
sls_load_vmobject(struct vm_object_info *obj, struct sls_channel *chan)
{
	int error;

	error = sls_read(obj, sizeof(*obj), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	if (obj->magic == SLS_OBJECTS_END)
	    return 0;

	if (obj->magic != SLS_OBJECT_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	if (obj->type == OBJT_VNODE) {
	    error = sls_load_path(&obj->path, chan);
	    if (error != 0)
		return error;
	}

	return 0;
}

int
sls_load_vmentry(struct vm_map_entry_info *entry, struct sls_channel *chan)
{
	int error;

	error = sls_read(entry, sizeof(*entry), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	if (entry->magic != SLS_ENTRY_INFO_MAGIC) {
	    SLS_DBG("magic mismatch");
	    return EINVAL;
	}

	return 0;
}

int 
sls_load_memory(struct memckpt_info *memory, struct sls_channel *chan)
{
	int error;

	error = sls_read(memory, sizeof(*memory), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	if (memory->vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	return 0;
}

int
sls_load_path(struct sbuf **sbp, struct sls_channel *chan) 
{
	int error;
	size_t len;
	char *path = NULL;
	int magic;
	struct sbuf *sb = NULL;


	error = sls_read(&magic, sizeof(magic), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	if (magic != SLS_STRING_MAGIC)
	    return EINVAL;

	error = sls_read(&len, sizeof(len), SLOSREC_PROC, chan);
	if (error != 0)
	    return error;

	/* First copy the data into a temporary raw buffer */
	path = malloc(len + 1, M_SLSMM, M_WAITOK);
	error = sls_read(path, len, SLOSREC_PROC, chan);
	if (error != 0)
	    goto error;
	path[len++] = '\0';

	sb = sbuf_new_auto();
	if (sb == NULL)
	    goto error;

	/* Then move it over to the sbuf */
	error = sbuf_bcpy(sb, path, len);
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

static int
sls_load_ptable_file(struct sls_pagetable *ptable, struct file *fp)
{
	int error = 0;
	void *hashpage;
	vm_offset_t vaddr;
	struct dump_page *new_entry;
	size_t size;

	error = htable_init(ptable);
	if (error != 0)
	    return error;

	for (;;) {

	    error = sls_file_read(&vaddr, sizeof(vaddr), fp);
	    if (error != 0)
		goto error;

	    /* Sentinel value */
	    if (vaddr == SLS_MSG_END)
		break;

	    error = sls_file_read(&size, sizeof(size), fp);
	    if (error != 0)
		goto error;

	    /*
	    * We assume that asking for a page-size chunk will
	    * give us a page-aligned chunk, which makes sense given
	    * that we are probably getting it from a slab allocator.
	    */
	    for (vm_offset_t off = 0; off < size; off += PAGE_SIZE) {
		new_entry = malloc(sizeof(*new_entry), M_SLSMM, M_WAITOK);
		hashpage = malloc(PAGE_SIZE, M_SLSMM, M_WAITOK);

		new_entry->vaddr = vaddr + off;
		new_entry->data = hashpage;
		error = sls_file_read(hashpage, PAGE_SIZE, fp);
		if (error != 0) {
		    free(new_entry, M_SLSMM);
		    free(hashpage, M_SLSMM);
		    goto error;
		}

		/*
		* Add the new page to the hash table, if it already 
		* exists there don't replace it.
		*/
		addpage_noreplace(ptable, new_entry);
	    }
	}

	return 0;

error:

	htable_fini(ptable);

	return error;
}

static int
sls_load_ptable_slos(struct sls_pagetable *ptable, struct slos_vnode *vp)
{
	struct dump_page *new_entry;
	uint8_t *buf, *hashpage;
	uint64_t offset, len;
	vm_offset_t vaddr;
	struct iovec aiov;
	struct uio auio;
	uint64_t rno;
	int error;

	error = htable_init(ptable);
	if (error != 0)
	    return error;


	/* Get the SLOS record with the metadata. */
	/* 
	 * XXX When we have multiple checkpoints we are
	 * going to have to look to the _last_ record of
	 * that type, but now we want to start looking from
	 * the beginning of the file for performance.
	 */
	error = slos_firstrno_typed(vp, SLOSREC_DATA, &rno);
	if (error != 0) {
	    htable_fini(ptable);
	    return error;
	}


	offset = 0;
	for (;;) {
	    /* Seek the next extent in the record. */
	    error = slos_rseek(vp, rno, offset, 0, &offset, &len);
	    if (error != 0) {
		htable_fini(ptable);
		printf("Seek failed\n");
		break;
	    }

	    /* If we get EOF, we're done. */
	    if (len == SREC_SEEKEOF)
		break;

	    /* Otherwise allocate a buffer for the data. */
	    buf = malloc(len, M_SLSMM, M_WAITOK);

	    /* Create the UIO for the disk. */
	    aiov.iov_base = buf;
	    aiov.iov_len = len;
	    slos_uioinit(&auio, offset, UIO_READ, &aiov, 1);

	    /* The read itself. */
	    error = slos_rread(vp, rno, &auio);
	    if (error != 0) {
		free(buf, M_SLSMM);
		htable_fini(ptable);
		return error;
	    }

	    /* 
	     * The offset from which we read is the virtual address
	     * in the restored process in which it must be placed.
	     */
	    vaddr = offset;

	    /* Increment the offset by the amount of bytes read. */
	    offset += (len - auio.uio_resid);

	    /* Break down the read part into pages, enter into ptable. */
	    for (vm_offset_t off = 0; off < len; off += PAGE_SIZE) {
		new_entry = malloc(sizeof(*new_entry), M_SLSMM, M_WAITOK);
		hashpage = malloc(PAGE_SIZE, M_SLSMM, M_WAITOK);

		/* Copy the page into a new buffer to be given to the ptable. */
		new_entry->vaddr = vaddr + off;
		new_entry->data = hashpage;
		memcpy(hashpage, &buf[off], PAGE_SIZE);

		/*
		* Add the new page to the hash table, if it already 
		* exists there don't replace it.
		*/
		addpage_noreplace(ptable, new_entry);
	    }

	    free(buf, M_SLSMM);
	}

	
	return 0;


}

int 
sls_load_ptable(struct sls_pagetable *ptable, struct sls_channel *chan)
{ 
	int error;

	switch (chan->type) {
	case SLS_FILE:
	    error = sls_load_ptable_file(ptable, chan->fp);
	    break;

	case SLS_OSD:
	    error = sls_load_ptable_slos(ptable, chan->vp);
	    break;

	default:
	    return EINVAL;
	}

	return error;
}
