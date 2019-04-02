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
#include "sls_snapshot.h"
#include "sls_file.h"


struct dump *
alloc_dump() {
	struct dump *dump = NULL;
	dump = malloc(sizeof(struct dump), M_SLSMM, M_NOWAIT);
	if (dump == NULL)
	    return NULL;
	dump->threads = NULL;
	dump->memory.entries = NULL;
	dump->filedesc.infos = NULL;
	dump->filedesc.cdir = NULL;
	dump->filedesc.rdir = NULL;
	dump->magic = SLS_DUMP_MAGIC;

	return dump;
}

void
free_dump(struct dump *dump) {
	struct vm_map_entry_info *entries;
	struct file_info *file_infos;
	char *filename;
	struct vm_object_info *obj_info;
	int i;

	if (!dump)
	    return;
	if (dump->threads)
	    free(dump->threads, M_SLSMM);

	entries = dump->memory.entries;
	if (entries) {
	    for (i = 0; i < dump->memory.vmspace.nentries; i++) {
		obj_info = entries[i].obj_info;
		if (obj_info != NULL) {
		    if (obj_info->filename != NULL)
			free(obj_info->filename, M_SLSMM);
		    free(obj_info, M_SLSMM);
		}
	    }
	    free(entries, M_SLSMM);
	}

	free(dump->filedesc.cdir, M_SLSMM); 
	free(dump->filedesc.rdir, M_SLSMM); 

	file_infos = dump->filedesc.infos;
	if (file_infos) {
	    for (i = 0; i < dump->filedesc.num_files; i++) {
		filename = file_infos[i].filename;
		free(filename, M_SLSMM);
	    }
	    free(file_infos, M_SLSMM);
	}

	free(dump, M_SLSMM);
}

/*
 * XXX In case of failure we leak memory left and right, fix that. Not important
 * right now, though, since if something breaks here we have probably messed up
 * something important in the code, and we need to reboot anyway.
 */
struct sls_snapshot *
load_dump(int fd)
{
	int error = 0;
	void *hashpage;
	vm_offset_t vaddr;
	struct dump_page *new_entry;
	struct dump *dump;
	struct vm_map_entry_info *cur_entry;
	struct thread_info *threads;
	struct vm_map_entry_info *entries;
	struct file_info *files;
	struct vm_object_info *cur_obj;
	size_t thread_size, entry_size, file_size;
	struct sls_snapshot *slss;
	int numfiles, numthreads, numentries;
	int len;
	int i;

	slss = slss_init(NULL, SLS_CKPT_FULL);
	dump = slss->slss_dump;

	/* Every static part of struct dump has its own magic, just for safety */
	error = file_read(dump, sizeof(struct dump), fd);
	if (error != 0) {
	    printf("Error: cannot read proc_info\n");
	    goto load_dump_error;
	}

	if (dump->magic != SLS_DUMP_MAGIC) {
	    printf("Error: SLS_DUMP_MAGIC %x does not match\n", dump->magic);
	    goto load_dump_error;
	}

	if (dump->proc.magic != SLS_PROC_INFO_MAGIC) {
	    printf("Error: SLS_PROC_INFO_MAGIC %x does not match\n", dump->proc.magic);
	    goto load_dump_error;
	}

	if (dump->filedesc.magic != SLS_FILEDESC_INFO_MAGIC) {
	    printf("SLS_FILEDESC_INFO_MAGIC %x does not match\n", dump->filedesc.magic);
	    goto load_dump_error;
	}

	if (dump->memory.vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    printf("Error: SLS_VMSPACE_INFO_MAGIC does not match\n");
	    goto load_dump_error;
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

	dump->filedesc.cdir = malloc(dump->filedesc.cdir_len, M_SLSMM, M_WAITOK);
	dump->filedesc.rdir = malloc(dump->filedesc.rdir_len, M_SLSMM, M_WAITOK);

	/* Read in arrays of objects */

	error = file_read(dump->threads, thread_size, fd);
	if (error != 0) {
	    printf("Error: cannot read thread_info\n");
	    goto load_dump_error;
	}

	for (i = 0; i < numthreads; i++) {
	    if (dump->threads[i].magic != SLS_THREAD_INFO_MAGIC) {
		printf("Error: SLS_THREAD_INFO_MAGIC does not match\n");
		goto load_dump_error;
	    }
	}

	error = file_read(dump->filedesc.infos, file_size, fd);
	if (error != 0) {
	    printf("Error: cannot read file_info\n");
	    goto load_dump_error;
	}

	for (i = 0; i < numfiles; i++) {
	    if (dump->filedesc.infos[i].magic != SLS_FILE_INFO_MAGIC) {
		printf("Error: SLS_FILE_INFO_MAGIC does not match\n");
		goto load_dump_error;
	    }
	}

	error = file_read(entries, entry_size, fd);
	if (error != 0) {
	    printf("Error: cannot read vm_map_entry_info\n");
	    goto load_dump_error;
	}

	for (i = 0; i < numentries; i++) {
	    if (dump->memory.entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
		printf("Error: SLS_ENTRY_INFO_MAGIC does not match\n");
		goto load_dump_error;
	    }
	}

	for (i = 0; i < numentries; i++) {

	    if (entries[i].obj_info == NULL)
		continue;

	    cur_obj = malloc(sizeof(struct vm_object_info), M_SLSMM, M_WAITOK);
	    if (cur_obj == NULL)
		goto load_dump_error;

	    entries[i].obj_info = cur_obj;

	    error = file_read(cur_obj, sizeof(struct vm_object_info), fd);
	    if (error != 0) {
		printf("Error: cannot read vm_object_info\n");
		goto load_dump_error;
	    }

	    if (cur_obj->magic != SLS_OBJECT_INFO_MAGIC) {
		printf("Error: SLS_OBJECT_INFO_MAGIC does not match\n");
		goto load_dump_error;
	    }
	}

	error = file_read(dump->filedesc.cdir, dump->filedesc.cdir_len, fd);
	if (error != 0) {
	    printf("Error: cannot read filedesc.cdir\n");
	    goto load_dump_error;
	}

	error = file_read(dump->filedesc.rdir, dump->filedesc.rdir_len, fd);
	if (error != 0) {
	    printf("Error: cannot read filedesc.rdir\n");
	    goto load_dump_error;
	}


	/* TODO: Split this up somehow */
	for (i = 0; i < numfiles; i++) {
	    len = files[i].filename_len;
	    files[i].filename = malloc(len, M_SLSMM, M_WAITOK);

	    error = file_read(files[i].filename, len, fd);
	    if (error != 0) {
		printf("Error: cannot read filename\n");
		goto load_dump_error;
	    }
	}


	/* 
	 * XXX Objects and entries will be decoupled when we start
	 * checkpointing whole object trees, but for now the code
	 * below suffices.
	 */
	for (i = 0; i < numentries; i++) {

	    cur_entry = &dump->memory.entries[i];
	    cur_obj = cur_entry->obj_info;
	    if (cur_obj == NULL || cur_obj->filename == NULL)
		continue;


	    cur_obj->filename = malloc(cur_obj->filename_len, M_SLSMM, M_WAITOK);
	    if(cur_obj->filename == NULL) {
		printf("Error: Allocation for filename failed\n");
		goto load_dump_error;
	    }

	    error = file_read(cur_obj->filename, cur_obj->filename_len, fd);
	    if (error != 0) {
		printf("Error: cannot read filename\n");
		goto load_dump_error;
	    }

	}


	for (;;) {

	    error = file_read(&vaddr, sizeof(vm_offset_t), fd);
	    if (error != 0) {
		printf("Error: cannot vm_offset_t\n");
		goto load_dump_error;
	    }

	    /* Sentinel value */
	    if (vaddr == ULONG_MAX)
		break;

	    /*
	    * We assume that asking for a page-size chunk will
	    * give us a page-aligned chunk, which makes sense given
	    * that we are probably getting it from a slab allocator.
	    */
	    new_entry = malloc(sizeof(*new_entry), M_SLSMM, M_WAITOK);
	    hashpage = malloc(PAGE_SIZE, M_SLSMM, M_WAITOK);

	    new_entry->vaddr = vaddr;
	    new_entry->data = hashpage;
	    error = file_read(hashpage, PAGE_SIZE, fd);
	    if (error != 0) {
		printf("Error: reading data failed\n");
		free(new_entry, M_SLSMM);
		free(hashpage, M_SLSMM);
		goto load_dump_error;
	    }

	    /*
	    * Add the new page to the hash table, if it already 
	    * exists there don't replace it.
	    */
	    slss_addpage_noreplace(slss, new_entry);
	}


	return slss;


load_dump_error:

	slss_fini(slss);
	return NULL;
}


static int
store_pages_file(vm_offset_t vaddr, vm_offset_t vaddr_data, int fd)
{
	int error; 

	error = file_write(&vaddr, sizeof(vm_offset_t), fd);
	if (error != 0) {
	    printf("Error: writing vm_map_entry_info failed\n");
	    return error;
	}
	
	/* XXX parallelize the uio, handle superpages */
	error = file_write((void*) vaddr_data, PAGE_SIZE, fd);
	if (error != 0)
	    return error;

	return 0;
}

static int
store_pages_mem(vm_offset_t vaddr, vm_offset_t vaddr_data,
	struct sls_snapshot *slss)
{
	struct dump_page *new_entry;
	void *hashpage;

	hashpage = malloc(PAGE_SIZE, M_SLSMM, M_WAITOK);
	memcpy(hashpage, (void *) vaddr_data, PAGE_SIZE);

	new_entry = malloc(sizeof(*new_entry), M_SLSMM, M_WAITOK);
	new_entry->vaddr = vaddr;
	new_entry->data = hashpage;

	slss_addpage_noreplace(slss, new_entry);

	return 0;
}

int
store_pages(struct vmspace *vm, struct sls_store_tgt tgt, int mode)
{
	vm_offset_t sentinel = ULONG_MAX;
	vm_offset_t vaddr, vaddr_data;
	struct vm_map_entry *entry;
	struct vm_map *map;
	vm_offset_t offset;
	vm_object_t obj;
	vm_page_t page;
	int error;
	
	map = &vm->vm_map;
	for (entry = map->header.next; entry != &map->header; entry = entry->next) {

	    offset = entry->offset;
	    obj = entry->object.vm_object;

	    while (obj != NULL) {
		if (obj->type != OBJT_DEFAULT)
		    break;

		TAILQ_FOREACH(page, &obj->memq, listq) {
	    
		    KASSERT(page != NULL, "page is NULL\n");
		    
		    vaddr = IDX_TO_VADDR(page->pindex, entry->start, offset);
		    
		    /* Never fails on amd64, check is here for futureproofing */
		    vaddr_data = userpage_map(page->phys_addr);
		    if ((void *) vaddr_data == NULL) {
			printf("Mapping page failed\n");
			return error;
		    }

		    if (tgt.type == SLS_FILE)
			error = store_pages_file(vaddr, vaddr_data, tgt.fd);
		    else
			error = store_pages_mem(vaddr, vaddr_data, tgt.slss);
		    
		    userpage_unmap(vaddr_data);
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
	if (error != 0) {
	    printf("Error: Writing sentinel page value failed with %d\n", error);
	    return error;
	}
	
	return 0;

}

int
store_dump(struct sls_snapshot *slss, int mode, struct vmspace *vm, int fd)
{
	int i;
	int error = 0;
	struct vm_map_entry_info *entries;
	struct thread_info *thread_infos;
	struct file_info *file_infos;
	size_t cdir_len, rdir_len;
	struct vm_object_info *cur_obj;
	struct sls_store_tgt tgt;
	int numthreads, numentries, numfiles;
	struct dump *dump;

	dump = slss->slss_dump;
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
	if (error != 0) {
	    printf("Error: Writing dump failed with code %d\n", error);
	    return error;
	}

	error = file_write(thread_infos, sizeof(struct thread_info) * numthreads, fd);
	if (error != 0) {
	    printf("Error: Writing thread info dump failed with code %d\n", error);
	    return error;
	}

	error = file_write(file_infos, sizeof(struct file_info) * numfiles, fd);
	if (error != 0) {
	    printf("Error: Writing file info dump failed with code %d\n", error);
	    return error;
	}

	error = file_write(entries, sizeof(*entries) * numentries, fd);
	if (error != 0) {
	    printf("Error: Writing entry info dump failed with code %d\n", error);
	    return error;
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;
	    if (cur_obj == NULL)
		continue;

	    error = file_write(cur_obj, sizeof(*cur_obj), fd);
	    if (error != 0) {
		printf("Error: Writing object info failed with code %d\n", error);
		return error;
	    }

	}

	cdir_len = dump->filedesc.cdir_len;
	error = file_write(dump->filedesc.cdir, cdir_len, fd);
	if (error != 0) {
	    printf("Error: Writing cdir path failed with code %d\n", error);
	    return error;
	}

	rdir_len = dump->filedesc.rdir_len;
	error = file_write(dump->filedesc.rdir, rdir_len, fd);
	if (error != 0) {
	    printf("Error: Writing cdir path failed with code %d\n", error);
	    return error;
	}


	for (i = 0; i < numfiles; i++) {
	    error = file_write(file_infos[i].filename, file_infos[i].filename_len, fd);
	    if (error != 0) {
		printf("Error: Writing filename failed with code %d\n", error);
		return error;
	    }
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;

	    if (cur_obj != NULL && cur_obj->filename != NULL) {
		error = file_write(cur_obj->filename, cur_obj->filename_len, fd);
		if (error != 0) {
		    printf("Error: Could not write filename\n");
		    return error;
		}
	    }
	}
	
	tgt = (struct sls_store_tgt) {
	    .type = SLS_FILE,
	    .fd	  = fd,
	};

	error = store_pages(vm, tgt, mode);
	if (error != 0) {
	    printf("Error: Dumping pages failed with %d\n", error);
	    return error;
	}

	return 0;
}
