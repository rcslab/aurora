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

/* XXX Will become a sysctl value */
size_t sls_contig_limit = 2 * 1024 * 1024;

#define SLS_MSG_END ULONG_MAX

static int
sls_store_path(struct sbuf *sb, int fd) 
{
    int error;
    char *path;
    size_t len;

    path = sbuf_data(sb);
    len = sbuf_len(sb);
    if (len < 0)
	return -1;

    error = file_write(&len, sizeof(len), fd);
    if (error != 0)
	return error;

    error = file_write(path, len, fd);
    if (error != 0)
	return error;

    return 0;
}

static int
sls_load_path(struct sbuf **sbp, int fd) 
{
    int error;
    size_t len;
    char *path = NULL;
    struct sbuf *sb = NULL;

    error = file_read(&len, sizeof(len), fd);
    if (error != 0)
	return error;

    path = malloc(len + 1, M_SLSMM, M_WAITOK);
    error = file_read(path, len, fd);
    if (error != 0)
	goto load_path_error;
    path[len++] = '\0';

    sb = sbuf_new_auto();
    if (sb == NULL)
	goto load_path_error;

    error = sbuf_bcpy(sb, path, len);
    if (error != 0)
	goto load_path_error;

    error = sbuf_finish(sb);
    if (error != 0)
	goto load_path_error;

    *sbp = sb;

    return 0;

load_path_error:

    if (sb != NULL)
	sbuf_delete(sb);

    free(path, M_SLSMM);
    *sbp = NULL;
    return error;

}


static int 
dump_init_htable(struct dump *dump)
{
	dump->pages = hashinit(HASH_MAX, M_SLSMM, &dump->hashmask);
	if (dump->pages == NULL)
		return ENOMEM;

	return 0;
}

static void
dump_fini_htable(struct dump *dump)
{
	int i;
	struct dump_page *cur_page;
	struct page_list *cur_bucket;

	for (i = 0; i <= dump->hashmask; i++) {
		cur_bucket = &dump->pages[i];
		while (!LIST_EMPTY(cur_bucket)) {
			cur_page = LIST_FIRST(cur_bucket);
			LIST_REMOVE(cur_page, next);
			free(cur_page->data, M_SLSMM);
			free(cur_page, M_SLSMM);
		}
	}

	hashdestroy(dump->pages, M_SLSMM, dump->hashmask);
}

struct dump *
alloc_dump()
{
	struct dump *dump;
	int error;

	dump = malloc(sizeof(struct dump), M_SLSMM, M_WAITOK | M_ZERO);
	dump->magic = SLS_DUMP_MAGIC;
	error = dump_init_htable(dump);
	if (error != 0) {
	    free(dump, M_SLSMM);
	    return NULL;
	}

	return dump;
}

void
free_dump(struct dump *dump)
{
	struct vm_map_entry_info *entries;
	struct file_info *file_infos;
	struct vm_object_info *obj_info;
	int i;

	if (dump == NULL)
	    return;

	dump_fini_htable(dump);

	if (dump->threads != NULL)
	    free(dump->threads, M_SLSMM);

	entries = dump->memory.entries;
	if (entries) {
	    for (i = 0; i < dump->memory.vmspace.nentries; i++) {
		obj_info = entries[i].obj_info;
		if (obj_info != NULL) {
		    if (obj_info->path != NULL)
			sbuf_delete(obj_info->path);
		    obj_info->path = NULL;
		}
		free(obj_info, M_SLSMM);
	    }
	    free(entries, M_SLSMM);
	}


	if (dump->filedesc.cdir != NULL) 
	    sbuf_delete(dump->filedesc.cdir);
	dump->filedesc.cdir = NULL;

	if (dump->filedesc.rdir != NULL) 
	    sbuf_delete(dump->filedesc.rdir);
	dump->filedesc.rdir = NULL;

	file_infos = dump->filedesc.infos;
	if (file_infos != 0) {
	    for (i = 0; i < dump->filedesc.num_files; i++) {
		if (file_infos[i].path != NULL) 
		    sbuf_delete(file_infos[i].path);
		file_infos[i].path = NULL;
	    }

	    free(file_infos, M_SLSMM);
	}

	free(dump, M_SLSMM);
}

void
addpage_noreplace(struct dump *dump, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;
	int already_there;

	vaddr = new_entry->vaddr;
	page_bucket = &dump->pages[vaddr & dump->hashmask];

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
struct dump *
sls_load(int fd)
{
	int error = 0;
	void *hashpage;
	vm_offset_t vaddr;
	struct dump_page *new_entry;
	struct dump *dump;
	struct thread_info *threads;
	struct vm_map_entry_info *entries;
	struct file_info *files;
	struct vm_object_info *cur_obj;
	size_t thread_size, entry_size, file_size;
	int numfiles, numthreads, numentries;
	size_t size;
	int i;

	dump = alloc_dump();

	error = file_read(dump, sizeof(struct dump), fd);
	if (error != 0)
	    goto sls_load_error;

	if (dump->magic != SLS_DUMP_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    goto sls_load_error;
	}

	if (dump->proc.magic != SLS_PROC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    goto sls_load_error;
	}

	if (dump->filedesc.magic != SLS_FILEDESC_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    goto sls_load_error;
	}

	if (dump->memory.vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    SLS_DBG("magic mismatch\n");
	    goto sls_load_error;
	}

	/* Allocations for array-like elements of the dump and cdir/rdir */
	numthreads = dump->proc.nthreads;
	numfiles = dump->filedesc.num_files;
	numentries = dump->memory.vmspace.nentries;

	thread_size = sizeof(struct thread_info) * numthreads; 
	file_size = sizeof(struct file_info) * numfiles; 
	entry_size = sizeof(struct vm_map_entry_info) * numentries; 

	threads = malloc(thread_size, M_SLSMM, M_WAITOK);
	files = malloc(file_size, M_SLSMM, M_WAITOK);
	entries = malloc(entry_size, M_SLSMM, M_WAITOK);

	dump->threads = threads;
	dump->filedesc.infos = files;
	dump->memory.entries = entries;

	/* Read in arrays of objects */

	error = file_read(dump->threads, thread_size, fd);
	if (error != 0)
	    goto sls_load_error;

	for (i = 0; i < numthreads; i++) {
	    if (dump->threads[i].magic != SLS_THREAD_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		goto sls_load_error;
	    }
	}

	error = file_read(dump->filedesc.infos, file_size, fd);
	if (error != 0)
	    goto sls_load_error;

	for (i = 0; i < numfiles; i++) {
	    if (dump->filedesc.infos[i].magic != SLS_FILE_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		goto sls_load_error;
	    }
	}

	error = sls_load_path(&dump->filedesc.cdir, fd);
	if (error != 0)
	    goto sls_load_error;

	error = sls_load_path(&dump->filedesc.rdir, fd);
	if (error != 0)
	    goto sls_load_error;

	for (i = 0; i < numfiles; i++) {
	    error = sls_load_path(&files[i].path, fd);
	    if (error != 0)
		goto sls_load_error;
	}

	error = file_read(entries, entry_size, fd);
	if (error != 0)
	    goto sls_load_error;

	for (i = 0; i < numentries; i++) {
	    if (dump->memory.entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
		SLS_DBG("magic mismatch");
		goto sls_load_error;
	    }
	}

	for (i = 0; i < numentries; i++) {

	    if (entries[i].obj_info == NULL)
		continue;

	    cur_obj = malloc(sizeof(struct vm_object_info), M_SLSMM, M_WAITOK);
	    if (cur_obj == NULL)
		goto sls_load_error;

	    entries[i].obj_info = cur_obj;

	    error = file_read(cur_obj, sizeof(struct vm_object_info), fd);
	    if (error != 0)
		goto sls_load_error;

	    if (cur_obj->magic != SLS_OBJECT_INFO_MAGIC) {
		SLS_DBG("magic mismatch\n");
		goto sls_load_error;
	    }

	    if (cur_obj->type == OBJT_VNODE) {
		error = sls_load_path(&cur_obj->path, fd);
		if (error != 0)
		    goto sls_load_error;
	    }
	}


	for (;;) {

	    error = file_read(&vaddr, sizeof(vaddr), fd);
	    if (error != 0)
		goto sls_load_error;

	    /* Sentinel value */
	    if (vaddr == SLS_MSG_END)
		break;

	    error = file_read(&size, sizeof(size), fd);
	    if (error != 0)
		goto sls_load_error;

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
		error = file_read(hashpage, PAGE_SIZE, fd);
		if (error != 0) {
		    free(new_entry, M_SLSMM);
		    free(hashpage, M_SLSMM);
		    goto sls_load_error;
		}

		/*
		* Add the new page to the hash table, if it already 
		* exists there don't replace it.
		*/
		addpage_noreplace(dump, new_entry);
	    }
	}


	return dump;


sls_load_error:

	free_dump(dump);
	return NULL;
}


static int
store_pages_file(vm_offset_t vaddr, size_t size, vm_offset_t data, int fd)
{
	int error; 

	error = file_write(&vaddr, sizeof(vaddr), fd);
	if (error != 0)
	    return error;

	error = file_write(&size, sizeof(size), fd);
	if (error != 0)
	    return error;
	
	error = file_write((void*) data, size, fd);
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

	file_write(&sentinel, sizeof(sentinel), tgt.fd);
	if (error != 0)
	    return error;

	return 0;

}

int
sls_store(struct dump *dump, int mode, struct vmspace *vm, int fd)
{
	int i;
	int error = 0;
	struct vm_map_entry_info *entries;
	struct thread_info *thread_infos;
	struct file_info *file_infos;
	struct vm_object_info *cur_obj;
	struct sls_store_tgt tgt;
	int numthreads, numentries, numfiles;

	thread_infos = dump->threads;
	entries = dump->memory.entries;
	file_infos = dump->filedesc.infos;

	numthreads = dump->proc.nthreads;
	numentries = dump->memory.vmspace.nentries;
	numfiles = dump->filedesc.num_files;

	for (i = 0; i < numentries; i++) {
	    if (entries->eflags & MAP_ENTRY_IS_SUB_MAP) {
		printf("WARNING: Submap entry found, dump will be wrong\n");
		continue;
	    }
	}

	error = file_write(dump, sizeof(struct dump), fd);
	if (error != 0)
	    return error;

	error = file_write(thread_infos, sizeof(struct thread_info) * numthreads, fd);
	if (error != 0)
	    return error;

	error = file_write(file_infos, sizeof(struct file_info) * numfiles, fd);
	if (error != 0)
	    return error;

	error = sls_store_path(dump->filedesc.cdir, fd);
	if (error != 0)
	    return error;

	error = sls_store_path(dump->filedesc.rdir, fd);
	if (error != 0)
	    return error;


	for (i = 0; i < numfiles; i++) {
	    error = sls_store_path(file_infos[i].path, fd);
	    if (error != 0)
		return error;
	}
	
	error = file_write(entries, sizeof(*entries) * numentries, fd);
	if (error != 0)
	    return error;

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;
	    if (cur_obj == NULL)
		continue;

	    error = file_write(cur_obj, sizeof(*cur_obj), fd);
	    if (error != 0)
		return error;

	    if (cur_obj->type == OBJT_VNODE) {
		error = sls_store_path(cur_obj->path, fd);
		if (error != 0)
		    return error;
	    }
	}

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

