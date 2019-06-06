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

/* Functions that load parts of the state. */
int 
sls_load_cpustate(struct proc_info *proc_info, struct thread_info **thread_infos, struct file *fp)
{
	int numthreads;
	size_t thread_size;
	struct thread_info *td_info = NULL;
	int error, i;

	error = sls_file_read(proc_info, sizeof(*proc_info), fp);
	if (error != 0)
	    goto sls_load_cpustate_error;

	if (proc_info->magic != SLS_PROC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    error = EINVAL;
	    goto sls_load_cpustate_error;
	}

	/* Allocate and read threads */
	numthreads = proc_info->nthreads;
	thread_size = sizeof(struct thread_info) * numthreads; 
	td_info = malloc(thread_size, M_SLSMM, M_WAITOK);

	error = sls_file_read(td_info, thread_size, fp);
	if (error != 0)
	    goto sls_load_cpustate_error;

	for (i = 0; i < numthreads; i++) {
	    if (td_info[i].magic != SLS_THREAD_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		error = EINVAL;
		goto sls_load_cpustate_error;
	    }
	}

	*thread_infos = td_info;

	return 0;

sls_load_cpustate_error:

	free(td_info, M_SLSMM);
    
	return error;
	

}

/* 
 * XXX This function is sketchy, but will get normalized once the restore
 * happens incrementally and not all at once.
 */
int
sls_load_filedesc(struct filedesc_info *filedesc, struct file *fp)
{
	struct file_info tmp_file;
	char *path = NULL;
	char *data = NULL;
	size_t file_size;
	int numfiles;
	int error, i;
	size_t len;

	struct sbuf *tmp_sbuf = NULL;

	/* General file descriptor */
	error = sls_file_read(filedesc, sizeof(*filedesc), fp);
	if (error != 0)
	    goto sls_load_filedesc_error;

	if (filedesc->magic != SLS_FILEDESC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    error = EINVAL;
	    goto sls_load_filedesc_error;
	}

	/* Current and root directories */
	error = sls_load_path(&filedesc->cdir, fp);
	if (error != 0)
	    goto sls_load_filedesc_error;

	error = sls_load_path(&filedesc->rdir, fp);
	if (error != 0)
	    goto sls_load_filedesc_error;

	/* 
	 * Allocate and read files. We don't know a priori how many,
	 * so we read them into a buffer, counting them in the process.
	 * We deserialize them afterwards. This is quite the loop-de-loop,
	 * but we can avoid it if we restore each file descriptor as we
	 * read it.
	 */

	tmp_sbuf = sbuf_new_auto();
	path = malloc(PATH_MAX, M_SLSMM, M_WAITOK);

	for (i = 0; ; i++) {

	    error = sls_file_read(&tmp_file, sizeof(tmp_file), fp);
	    if (error != 0)
		goto sls_load_filedesc_error;

	    if (tmp_file.magic == SLS_FILES_END)
		break;

	    if(tmp_file.magic != SLS_FILE_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		error = EINVAL;
		goto sls_load_filedesc_error;
	    }

	    error = sbuf_bcat(tmp_sbuf, (void *) &tmp_file, sizeof(tmp_file));
	    if (error != 0)
		goto sls_load_filedesc_error;


	    error = sls_file_read(&len, sizeof(len), fp);
	    if (error != 0)
		goto sls_load_filedesc_error;

	    error = sbuf_bcat(tmp_sbuf, (void *) &len, sizeof(len));
	    if (error != 0)
		goto sls_load_filedesc_error;


	    error = sls_file_read(path, len, fp);
	    if (error != 0)
		goto sls_load_filedesc_error;

	    error = sbuf_bcat(tmp_sbuf, (void *) path, len);
	    if (error != 0)
		goto sls_load_filedesc_error;

	}

	free(path, M_SLSMM);
	path = NULL;

	sbuf_finish(tmp_sbuf);

	filedesc->num_files = numfiles = i;
	file_size = sizeof(struct file_info) * numfiles; 
	filedesc->infos = malloc(file_size, M_SLSMM, M_WAITOK);

	data = sbuf_data(tmp_sbuf);

	for (i = 0; i < numfiles; i++) {
	    memcpy(&filedesc->infos[i], data, sizeof(filedesc->infos[i]));
	    data += sizeof(filedesc->infos[i]);

	    if (filedesc->infos[i].magic != SLS_FILE_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		error = EINVAL;
		goto sls_load_filedesc_error;
	    }

	    memcpy((void *) &len, data, sizeof(len));
	    data += sizeof(len);

	    filedesc->infos[i].path = sbuf_new_auto();
	    sbuf_bcpy(filedesc->infos[i].path, data, len);
	    data += len;

	}

	sbuf_delete(tmp_sbuf);

	return 0;

sls_load_filedesc_error:

	free(path, M_SLSMM);
	if (tmp_sbuf != NULL)
	    sbuf_delete(tmp_sbuf);

	return error;

}

int 
sls_load_memory(struct memckpt_info *memory, struct file *fp)
{
	struct vm_map_entry_info *entries;
	struct vm_object_info *cur_obj;
	size_t entry_size;
	int numentries;
	int error, i;

	/* Allocate and read memory structure */
	error = sls_file_read(memory, sizeof(*memory), fp);
	if (error != 0)
	    goto sls_load_memory_error;

	if (memory->vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    error = EINVAL;
	    goto sls_load_memory_error;
	}

	/* Allocations for array-like elements of the dump and cdir/rdir */
	numentries = memory->vmspace.nentries;
	entry_size = sizeof(struct vm_map_entry_info) * numentries; 
	entries = malloc(entry_size, M_SLSMM, M_WAITOK);
	memory->entries = entries;

	for (i = 0; i < numentries; i++) {
	    error = sls_file_read(&entries[i], sizeof(entries[i]), fp);
	    if (error != 0)
		goto sls_load_memory_error;

	    if (memory->entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
		SLS_DBG("magic mismatch");
		error = EINVAL;
		goto sls_load_memory_error;
	    }

	    if (entries[i].obj_info == NULL)
		continue;

	    cur_obj = malloc(sizeof(struct vm_object_info), M_SLSMM, M_WAITOK);
	    if (cur_obj == NULL)
		goto sls_load_memory_error;

	    entries[i].obj_info = cur_obj;

	    error = sls_file_read(cur_obj, sizeof(struct vm_object_info), fp);
	    if (error != 0)
		goto sls_load_memory_error;

	    if (cur_obj->magic != SLS_OBJECT_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		error = EINVAL;
		goto sls_load_memory_error;
	    }

	    if (cur_obj->type == OBJT_VNODE) {
		error = sls_load_path(&cur_obj->path, fp);
		if (error != 0)
		    goto sls_load_memory_error;
	    }
	}

	return 0;

sls_load_memory_error:


	return error;
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


static int
store_pages_file(vm_offset_t vaddr, size_t size, vm_offset_t data, int fd)
{
	int error; 

	error = sls_fd_write(&vaddr, sizeof(vaddr), fd);
	if (error != 0)
	    return error;

	error = sls_fd_write(&size, sizeof(size), fd);
	if (error != 0)
	    return error;
	
	error = sls_fd_write((void*) data, size, fd);
	if (error != 0)
	    return error;

	return 0;
}

static size_t
sls_contig_pages(vm_object_t obj, vm_page_t *page)
{
	vm_page_t prev, cur;
	size_t contig_size; 

	prev = *page;
	cur = TAILQ_NEXT(prev, listq);
	contig_size = pagesizes[prev->psind];

	TAILQ_FOREACH_FROM(cur, &obj->memq, listq) {
	    /* Pages need to be physically and logically contiguous. */
	    if (prev->phys_addr + pagesizes[prev->psind] != cur->phys_addr ||
		prev->pindex + OFF_TO_IDX(pagesizes[prev->psind]) != cur->pindex)
		break;
	
	    if (contig_size > sls_contig_limit)
		break;

	    contig_size += pagesizes[cur->psind];
	    prev = cur;

	}

	*page = cur;

	return contig_size;
}

static int
sls_store_pages(struct vmspace *vm, struct sls_store_tgt tgt, int mode)
{
	vm_offset_t sentinel = ULONG_MAX;
	vm_offset_t vaddr, data;
	struct vm_map_entry *entry;
	struct vm_map *map;
	vm_offset_t offset;
	vm_object_t obj;
	vm_page_t startpage, page;
	size_t contig_size;
	int error;
	
	map = &vm->vm_map;
	for (entry = map->header.next; entry != &map->header; entry = entry->next) {

	    offset = entry->offset;
	    obj = entry->object.vm_object;

	    for (;;) {
		if (obj == NULL || obj->type != OBJT_DEFAULT)
		    break;

		page = TAILQ_FIRST(&obj->memq); 
		while (page != NULL) {
		    startpage = page;
		    vaddr = IDX_TO_VADDR(startpage->pindex, entry->start, offset);
		    contig_size = sls_contig_pages(obj, &page);

		    data = pmap_map(NULL, startpage->phys_addr, 
			    startpage->phys_addr + contig_size, 
			    VM_PROT_READ | VM_PROT_WRITE);
		    if (data == 0)
			return ENOMEM;

		    error = store_pages_file(vaddr, contig_size, data, tgt.fd);
		    userpage_unmap(data);

		    if (error != 0)
			return error;

		    /* We have looped around */
		    if (page == NULL || 
			startpage->pindex >= page->pindex)
			break;
		}

		if (mode == SLS_CKPT_DELTA)
		    break;

		offset += obj->backing_object_offset;
		obj = obj->backing_object;
	    }
	}

	if (tgt.type == SLS_MEM)
	    return 0;

	sls_fd_write(&sentinel, sizeof(sentinel), tgt.fd);
	if (error != 0)
	    return error;

	return 0;

}

int
sls_store(struct sls_process *slsp, int mode, int fd)
{
	int error = 0;
	struct sls_store_tgt tgt;
	struct vmspace *vm;
	char *buf;
	size_t len;

	vm = slsp->slsp_vm;

	/* First write the coalesced metadata */
	/* 
	 * XXX Unpack them maybe to be able to
	 * store semantic information alongside them.
	 * Not useful for the file, but necessary for
	 * the OSD.
	 */
	error = sbuf_finish(slsp->slsp_ckptbuf);
	if (error != 0)
	    return error;

	buf = sbuf_data(slsp->slsp_ckptbuf);
	len = sbuf_len(slsp->slsp_ckptbuf);

	error = sls_fd_write(buf, len, fd);

	sbuf_clear(slsp->slsp_ckptbuf);

	if(error != 0)
	    return error;

	tgt = (struct sls_store_tgt) {
	    .type = SLS_FILE,
	    .fd	  = fd,
	};

	error = sls_store_pages(vm, tgt, mode);
	if (error != 0) {
	    printf("Error: Dumping pages failed with %d\n", error);
	    return error;
	}

	return 0;
}

