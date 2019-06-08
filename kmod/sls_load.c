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
#include "sls_dump.h"
#include "sls_ioctl.h"
#include "sls_file.h"
#include "sls_path.h"

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


void
addpage_noreplace(struct sls_pagetable *ptable, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;
	int already_there;

	vaddr = new_entry->vaddr;
	page_bucket = &ptable->pages[vaddr & ptable->hashmask];

	already_there = 0;
	LIST_FOREACH(page_entry, page_bucket, next) {
	    if(page_entry->vaddr == new_entry->vaddr) {
		free(new_entry->data, M_SLSMM);
		free(new_entry, M_SLSMM);
		already_there = 1;
		break;
	    }
	}

	if (already_there == 0)
	    LIST_INSERT_HEAD(page_bucket, new_entry, next);
}

int
sls_load_thread(struct thread_info *thread_info, struct file *fp)
{
	int error;

	error = sls_file_read(thread_info, sizeof(*thread_info), fp);
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
sls_load_proc(struct proc_info *proc_info, struct file *fp)
{
	int error;

	error = sls_file_read(proc_info, sizeof(*proc_info), fp);
	if (error != 0)
	    return error;

	if (proc_info->magic != SLS_PROC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	return 0;
}

int
sls_load_file(struct file_info *file_info, struct file *fp)
{
	int error;

	error = sls_file_read(file_info, sizeof(*file_info), fp);
	if (error != 0)
	    return error;

	if (file_info->magic == SLS_FILES_END)
	    return 0;

	if (file_info->magic != SLS_FILE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch");
	    return EINVAL;
	}

	error = sls_load_path(&file_info->path, fp);
	if (error != 0)
	    return error;

	return 0;
}

int
sls_load_filedesc(struct filedesc_info *filedesc, struct file *fp)
{
	int error;

	/* General file descriptor */
	error = sls_file_read(filedesc, sizeof(*filedesc), fp);
	if (error != 0)
	    return 0;

	if (filedesc->magic != SLS_FILEDESC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	/* Current and root directories */
	error = sls_load_path(&filedesc->cdir, fp);
	if (error != 0)
	    return error;

	error = sls_load_path(&filedesc->rdir, fp);
	if (error != 0) {
	    sbuf_delete(filedesc->cdir);
	    return error;
	}

	return 0;
}

int
sls_load_vmobject(struct vm_object_info *obj, struct file *fp)
{
	int error;

	error = sls_file_read(obj, sizeof(*obj), fp);
	if (error != 0)
	    return error;

	if (obj->magic == SLS_OBJECTS_END)
	    return 0;

	if (obj->magic != SLS_OBJECT_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	if (obj->type == OBJT_VNODE) {
	    error = sls_load_path(&obj->path, fp);
	    if (error != 0)
		return error;
	}

	return 0;
}

int
sls_load_vmentry(struct vm_map_entry_info *entry, struct file *fp)
{
	int error;

	error = sls_file_read(entry, sizeof(*entry), fp);
	if (error != 0)
	    return error;

	if (entry->magic != SLS_ENTRY_INFO_MAGIC) {
	    SLS_DBG("magic mismatch");
	    return EINVAL;
	}

	return 0;
}

int 
sls_load_memory(struct memckpt_info *memory, struct file *fp)
{
	int error;

	error = sls_file_read(memory, sizeof(*memory), fp);
	if (error != 0)
	    return error;

	if (memory->vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    return EINVAL;
	}

	return 0;
}


int
sls_load_ptable(struct sls_pagetable *ptable, struct file *fp)
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
		goto sls_load_ptable_error;

	    /* Sentinel value */
	    if (vaddr == SLS_MSG_END)
		break;

	    error = sls_file_read(&size, sizeof(size), fp);
	    if (error != 0)
		goto sls_load_ptable_error;

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
		    goto sls_load_ptable_error;
		}

		/*
		* Add the new page to the hash table, if it already 
		* exists there don't replace it.
		*/
		addpage_noreplace(ptable, new_entry);
	    }
	}

	return 0;

sls_load_ptable_error:

	htable_fini(ptable);

	return error;
}
