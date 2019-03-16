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

#include "_slsmm.h"
#include "cpuckpt.h"
#include "dump.h"
#include "backends/fileio.h"
#include "hash.h"
#include "memckpt.h"
#include "slsmm.h"


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
		if (obj_info != NULL)
		    free(obj_info, M_SLSMM);
	    }
	    free(entries, M_SLSMM);
	}


	file_infos = dump->filedesc.infos;
	if (file_infos) {
	    for (i = 0; i < dump->filedesc.num_files; i++)
		filename = file_infos[i].filename;
	    free(file_infos, M_SLSMM);
	}

	free(dump, M_SLSMM);
}

/*
 * XXX In case of failure we leak memory left and right, fix that. Not important
 * right now, though, since if something breaks here we have probably messed up
 * something important in the code, and we need to reboot anyway.
 *
 * Alternatively, we can refactor so that all allocation happens in one place.
 * We need a global checkpoint size for that at the beginning of the checkpoint,
 * which I believe is completely feasible. We should do that later, though.
 */
int
load_dump(struct dump *dump, struct sls_desc *desc)
{
	int error = 0;
	void *hashpage;
	vm_offset_t vaddr;
	struct dump_page *page_entry, *new_entry;
	struct page_tailq *page_bucket;
	int already_there;
	struct vm_map_entry_info *cur_entry;
	struct thread_info *threads;
	struct vm_map_entry_info *entries;
	struct file_info *files;
	struct vm_object_info *cur_obj;
	size_t thread_size, entry_size, file_size;
	int numfiles, numthreads, numentries;
	int len;
	int i;

	/* TEMP 
	if (desc.type == DESC_MD)
	    md_reset(desc.index);
	*/

	/* Every static part of struct dump has its own magic, just for safety */
	error = fd_read(dump, sizeof(struct dump), desc);
	if (error != 0) {
	    printf("Error: cannot read proc_info\n");
	    return error;
	}

	if (dump->magic != SLS_DUMP_MAGIC) {
	    printf("Error: SLS_DUMP_MAGIC %x does not match\n", dump->magic);
	    return -1;
	}

	if (dump->proc.magic != SLS_PROC_INFO_MAGIC) {
	    printf("Error: SLS_PROC_INFO_MAGIC %x does not match\n", dump->proc.magic);
	    return -1;
	}

	if (dump->filedesc.magic != SLS_FILEDESC_INFO_MAGIC) {
	    printf("SLS_FILEDESC_INFO_MAGIC %x does not match\n", dump->filedesc.magic);
	    return -1;
	}

	if (dump->memory.vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
	    printf("Error: SLS_VMSPACE_INFO_MAGIC does not match\n");
	    return -1;
	}

	/* Allocations for array-like elements of the dump and cdir/rdir */
	numthreads = dump->proc.nthreads;
	numfiles = dump->filedesc.num_files;
	numentries = dump->memory.vmspace.nentries;

	thread_size = sizeof(struct thread_info) * numthreads; 
	file_size = sizeof(struct file_info) * numfiles; 
	entry_size = sizeof(struct vm_map_entry_info) * numentries; 

	threads = malloc(thread_size, M_SLSMM, M_NOWAIT);
	if (threads == NULL) {
	    printf("Error: cannot allocate thread_info\n");
	    return ENOMEM;
	}

	files = malloc(file_size, M_SLSMM, M_NOWAIT);
	if (files == NULL) {
	    printf("Allocation of file infos failed\n");
	    return ENOMEM;
	}

	entries = malloc(entry_size, M_SLSMM, M_NOWAIT);
	if (entries == NULL) {
	    printf("Error: cannot allocate entries\n");
	    return ENOMEM;
	}

	dump->threads = threads;
	dump->filedesc.infos = files;
	dump->memory.entries = entries;

	dump->filedesc.cdir = malloc(dump->filedesc.cdir_len, M_SLSMM, M_NOWAIT);
	dump->filedesc.rdir = malloc(dump->filedesc.rdir_len, M_SLSMM, M_NOWAIT);
	if((!dump->filedesc.cdir && dump->filedesc.cdir_len) ||
		(!dump->filedesc.rdir && dump->filedesc.rdir_len)) {
	    printf("Error: Allocation for cdir/rdir failed\n");
	    return ENOMEM;
	}

	/* Read in arrays of objects */

	error = fd_read(dump->threads, thread_size, desc);
	if (error != 0) {
	    printf("Error: cannot read thread_info\n");
	    return error;
	}

	for (i = 0; i < numthreads; i++) {
	    if (dump->threads[i].magic != SLS_THREAD_INFO_MAGIC) {
		printf("Error: SLS_THREAD_INFO_MAGIC does not match\n");
		return -1;
	    }
	}

	error = fd_read(dump->filedesc.infos, file_size, desc);
	if (error != 0) {
	    printf("Error: cannot read file_info\n");
	    return error;
	}

	for (i = 0; i < numfiles; i++) {
	    if (dump->filedesc.infos[i].magic != SLS_FILE_INFO_MAGIC) {
		printf("Error: SLS_FILE_INFO_MAGIC does not match\n");
		return -1;
	    }
	}

	error = fd_read(entries, entry_size, desc);
	if (error != 0) {
	    printf("Error: cannot read vm_map_entry_info\n");
	    return error;
	}

	for (i = 0; i < numentries; i++) {
	    if (dump->memory.entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
		printf("Error: SLS_ENTRY_INFO_MAGIC does not match\n");
		return -1;
	    }
	}

	for (i = 0; i < numentries; i++) {

	    if (entries[i].obj_info == NULL)
		continue;

	    cur_obj = malloc(sizeof(struct vm_object_info), M_SLSMM, M_NOWAIT);
	    if (cur_obj == NULL)
		return error;

	    entries[i].obj_info = cur_obj;

	    error = fd_read(cur_obj, sizeof(struct vm_object_info), desc);
	    if (error != 0) {
		printf("Error: cannot read vm_object_info\n");
		return error;
	    }

	    if (cur_obj->magic != SLS_OBJECT_INFO_MAGIC) {
		printf("Error: SLS_OBJECT_INFO_MAGIC does not match\n");
		return -1;
	    }
	}

	error = fd_read(dump->filedesc.cdir, dump->filedesc.cdir_len, desc);
	if (error != 0) {
	    printf("Error: cannot read filedesc.cdir\n");
	    return error;
	}

	error = fd_read(dump->filedesc.rdir, dump->filedesc.rdir_len, desc);
	if (error != 0) {
	    printf("Error: cannot read filedesc.rdir\n");
	    return error;
	}


	/* TODO: Split this up somehow */
	for (i = 0; i < numfiles; i++) {
	    len = files[i].filename_len;
	    files[i].filename = malloc(len, M_SLSMM, M_NOWAIT);
	    if (!files[i].filename) {
		printf("Error: Could not allocate space for filename\n");
		return ENOMEM;
	    }

	    error = fd_read(files[i].filename, len, desc);
	    if (error != 0) {
		printf("Error: cannot read filename\n");
		return error;
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


	    cur_obj->filename = malloc(cur_obj->filename_len, M_SLSMM, M_NOWAIT);
	    if(cur_obj->filename == NULL) {
		printf("Error: Allocation for filename failed\n");
		return -1;
	    }

	    error = fd_read(cur_obj->filename, cur_obj->filename_len, desc);
	    if (error != 0) {
		printf("Error: cannot read filename\n");
		return error;
	    }

	}


	for (;;) {

	    error = fd_read(&vaddr, sizeof(vm_offset_t), desc);
	    if (error != 0) {
		printf("Error: cannot vm_offset_t\n");
		return error;
	    }

	    /* Sentinel value */
	    if (vaddr == ULONG_MAX)
		break;

	    /*
	    * We assume that asking for a page-size chunk will
	    * give us a page-aligned chunk, which makes sense given
	    * that we are probably getting it from a slab allocator.
	    */
	    new_entry = malloc(sizeof(*new_entry), M_SLSMM, M_NOWAIT);
	    hashpage = malloc(PAGE_SIZE, M_SLSMM, M_NOWAIT);

	    if (!(hashpage && new_entry)) {
		printf("Error: Allocations failed\n");
		free(new_entry, M_SLSMM);
		free(hashpage, M_SLSMM);
		return -ENOMEM;
	    }

	    new_entry->vaddr = vaddr;
	    new_entry->data = hashpage;
	    error = fd_read(hashpage, PAGE_SIZE, desc);
	    if (error != 0) {
		printf("Error: reading data failed\n");
		free(new_entry, M_SLSMM);
		free(hashpage, M_SLSMM);
		return error;
	    }

	    /*
	    * Add the new page to the hash table, if it already 
	    * exists there don't replace it (we suppose we are
	    * calling load_dump() from the most recent dump to the
	    * oldest).
	    */
	    page_bucket = &slspages[vaddr & hashmask];

	    already_there = 0;
	    LIST_FOREACH(page_entry, page_bucket, next) {
		if(page_entry->vaddr == new_entry->vaddr) {
		    free(new_entry, M_SLSMM);
		    free(hashpage, M_SLSMM);
		    already_there = 1;
		    break;
		}
	    }

	    if (!already_there) {
		LIST_INSERT_HEAD(page_bucket, new_entry, next);
	    }
	}


	return 0;
}


struct dump *
compose_dump(struct sls_desc *descs, int ndescs)
{
	struct dump *dump;
	struct dump *currdump;
	int error = 0;

	dump = alloc_dump();
	currdump = alloc_dump();

	if (!dump || !currdump) {
	    printf("Error: cannot allocate dump struct at slsmm_restore\n");
	    goto error;
	}


	error = load_dump(dump, &descs[ndescs - 1]);
	if (error != 0) {
	    printf("Error: cannot load dumps\n");
	    goto error;
	}

	/*
	* We are constantly loading dumps because we only need the side-effect
	* of this action - populating the address space.
	*/
	for (int i = ndescs - 2; i >= 0; i--) {

	    /* Memory leak (the thread/entry arrays), will be fixed when we flesh out alloc_dump()*/
	    error = load_dump(currdump, &descs[i]);

	    /*
	    * XXX Inelegant, but the whole dump struct allocation procedure
	    * leaves a lot to be desired, so the whole thing will be
	    * refactored later.
	    */
	    free_dump(currdump);
	    currdump = alloc_dump();
	    if (!currdump) {
		printf("Error: cannot allocate dump struct at slsmm_restore\n");
		goto error;
	    }
	}

	free_dump(currdump);

	return dump;

error:
	free_dump(dump);
	free_dump(currdump);

	return NULL;
}

static void 
store_pages(struct sls_desc *desc)
{
    int i;
    struct dump_page *entry;

    for (i = 0; i <= hashmask; i++) {
	LIST_FOREACH(entry, &slspages[i & hashmask], next) {
	    /* XXX error handling */
	    fd_write(&entry->vaddr, sizeof(vm_offset_t), desc);
	    fd_write(entry->data, PAGE_SIZE, desc);
	}
    } 
}

int
store_dump(struct proc *p, struct dump *dump, vm_object_t *objects, int mode, struct sls_desc *desc)
{
	int i;
	int error = 0;
	struct vm_map_entry_info *entries;
	struct thread_info *thread_infos;
	struct file_info *file_infos;
	size_t cdir_len, rdir_len;
	struct vm_object_info *cur_obj;
	int numthreads, numentries, numfiles;
	vm_offset_t sentinel = ULONG_MAX;
	int pid;

	if (p != NULL)
		pid = p->p_pid;

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

	error = fd_write(dump, sizeof(struct dump), desc);
	if (error != 0) {
	    printf("Error: Writing dump failed with code %d\n", error);
	    return error;
	}

	error = fd_write(thread_infos, sizeof(struct thread_info) * numthreads, desc);
	if (error != 0) {
	    printf("Error: Writing thread info dump failed with code %d\n", error);
	    return error;
	}

	error = fd_write(file_infos, sizeof(struct file_info) * numfiles, desc);
	if (error != 0) {
	    printf("Error: Writing file info dump failed with code %d\n", error);
	    return error;
	}

	error = fd_write(entries, sizeof(*entries) * numentries, desc);
	if (error != 0) {
	    printf("Error: Writing entry info dump failed with code %d\n", error);
	    return error;
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;
	    if (cur_obj == NULL)
		continue;

	    error = fd_write(cur_obj, sizeof(*cur_obj), desc);
	    if (error != 0) {
		printf("Error: Writing object info failed with code %d\n", error);
		return error;
	    }

	}


	cdir_len = dump->filedesc.cdir_len;
	error = fd_write(dump->filedesc.cdir, cdir_len, desc);
	if (error != 0) {
	    printf("Error: Writing cdir path failed with code %d\n", error);
	    return error;
	}

	rdir_len = dump->filedesc.rdir_len;
	error = fd_write(dump->filedesc.rdir, rdir_len, desc);
	if (error != 0) {
	    printf("Error: Writing cdir path failed with code %d\n", error);
	    return error;
	}


	for (i = 0; i < numfiles; i++) {
	    error = fd_write(file_infos[i].filename, file_infos[i].filename_len, desc);
	    if (error != 0) {
		printf("Error: Writing filename failed with code %d\n", error);
		return error;
	    }
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;

	    if (cur_obj != NULL && cur_obj->filename != NULL) {
		error = fd_write(cur_obj->filename, cur_obj->filename_len, desc);
		if (error != 0) {
		    printf("Error: Could not write filename\n");
		    return error;
		}
	    }
	}

	/* XXX error handling */
	if (objects != NULL)
	    fd_dump(entries, objects, numentries, desc, mode);
	else
	    store_pages(desc);

	fd_write(&sentinel, sizeof(sentinel), desc);

	/* For delta dumps, deallocate the object to collapse the chain again. */
	for (i = 0; i < numentries; i++) {
	    if (objects[i] == NULL)
		continue;

	    if (pid_checkpointed[pid] && mode == SLSMM_CKPT_DELTA) {
		vm_object_deallocate(objects[i]->backing_object);
		vm_object_collapse(objects[i]);
	    }
	}

	return 0;
}
