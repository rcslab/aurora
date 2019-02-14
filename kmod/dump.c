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
#include "fileio.h"
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
	int i;

	if (!dump)
	    return;
	if (dump->threads)
	    free(dump->threads, M_SLSMM);

	entries = dump->memory.entries;
	if (entries) {
	    for (i = 0; i < dump->memory.vmspace.nentries; i++) {
		filename = dump->memory.entries[i].filename;
		if (filename)
		    free(filename, M_SLSMM);
	    }
	    free(entries, M_SLSMM);
	}

	if (dump->filedesc.cdir)
	    free(dump->filedesc.cdir, M_SLSMM);
	if (dump->filedesc.rdir)
	    free(dump->filedesc.rdir, M_SLSMM);

	file_infos = dump->filedesc.infos;
	if (file_infos) {
	    for (i = 0; i < dump->filedesc.num_files; i++) {
		filename = file_infos[i].filename;
		if (filename)
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
 *
 * Alternatively, we can refactor so that all allocation happens in one place.
 * We need a global checkpoint size for that at the beginning of the checkpoint,
 * which I believe is completely feasible. We should do that later, though.
 */
int
load_dump(struct dump *dump, struct sls_desc desc)
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
	size_t thread_size, entry_size, file_size;
	int len;
	int i, j;

	if (desc.type == DESC_MD)
	    md_reset(desc.index);

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

	thread_size = sizeof(struct thread_info) * dump->proc.nthreads;
	file_size = sizeof(struct file_info) * dump->filedesc.num_files;
	entry_size = sizeof(struct vm_map_entry_info) * dump->memory.vmspace.nentries;

	printf("Thread size is %lu\n", thread_size);
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

	for (i = 0; i < dump->proc.nthreads; i++) {
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

	for (i = 0; i < dump->filedesc.num_files; i++) {
	    if (dump->filedesc.infos[i].magic != SLS_FILE_INFO_MAGIC) {
		printf("Error: SLS_FILE_INFO_MAGIC does not match\n");
		return -1;
	    }
	}

	error = fd_read(dump->memory.entries, entry_size, desc);
	if (error != 0) {
	    printf("Error: cannot read vm_map_entry_info\n");
	    return error;
	}

	for (i = 0; i < dump->memory.vmspace.nentries; i++) {
	    if (dump->memory.entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
		printf("Error: SLS_ENTRY_INFO_MAGIC does not match\n");
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
	for (i = 0; i < dump->filedesc.num_files; i++) {
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



	for (i = 0; i < dump->memory.vmspace.nentries; i++) {

	    cur_entry = &dump->memory.entries[i];
	    if (cur_entry->filename_len == 0)
		continue;

	    cur_entry->filename = malloc(cur_entry->filename_len, M_SLSMM, M_NOWAIT);
	    if(cur_entry->filename == NULL) {
		printf("Error: Allocation for filename failed\n");
		return -1;
	    }

	    error = fd_read(cur_entry->filename, cur_entry->filename_len, desc);
	    if (error != 0) {
		printf("Error: cannot read filename\n");
		return error;
	    }

	}

	for (i = 0; i < dump->memory.vmspace.nentries; i++) {
	    cur_entry = &dump->memory.entries[i];

	    for (j = 0; j < cur_entry->resident_page_count; j++) {

		error = fd_read(&vaddr, sizeof(vm_offset_t), desc);
		if (error != 0) {
		    printf("Error: cannot vm_offset_t\n");
		    return error;
		}

		/*
		* We assume that asking for a page-size chunk will
		* give us a page-aligned chunk, which makes sense given
		* that we are probably getting it from a slab allocator.
		*/
		new_entry = malloc(sizeof(*page_entry), M_SLSMM, M_NOWAIT);
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

		if (!already_there)
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
	struct sls_desc cur_desc;

	dump = alloc_dump();
	currdump = alloc_dump();

	if (!dump || !currdump) {
	    printf("Error: cannot allocate dump struct at slsmm_restore\n");
	    goto error;
	}

	cur_desc = descs[ndescs - 1];

	error = load_dump(dump, cur_desc);
	if (error != 0) {
	    printf("Error: cannot load dumps\n");
	    goto error;
	}

	/*
	* We are constantly loading dumps because we only need the side-effect
	* of this action - populating the address space.
	*/
	/* XXX I think that's the wrong load order! */
	for (int i = 0; i < ndescs - 1; i++) {
	    cur_desc = descs[i];

	    /* Memory leak (the thread/entry arrays), will be fixed when we flesh out alloc_dump()*/
	    error = load_dump(currdump, cur_desc);

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


int
store_dump(struct dump *dump, vm_object_t *objects, long mode, struct sls_desc desc)
{
	int i;
	int error = 0;
	vm_page_t page;
	vm_offset_t vaddr, vaddr_data;
	struct vm_map_entry_info *entries, *cur_entry;
	struct thread_info *thread_infos;
	struct file_info *file_infos;
	size_t cdir_len, rdir_len;
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
	    cur_entry = &entries[i];

	    if (cur_entry->filename) {
		error = fd_write(cur_entry->filename, cur_entry->filename_len, desc);
		if (error != 0) {
		    printf("Error: Could not write filename\n");
		    return error;
		}
	    }
	}

	for (i = 0; i < numentries; i++) {

	    if (objects[i] == NULL)
		continue;

	    cur_entry = &entries[i];

	    TAILQ_FOREACH(page, &objects[i]->memq, listq) {

		/*
		* XXX Does this check make sense? We _are_ getting pages
		* from a valid object, after all, why would it have NULL
		* pointers in its list?
		*/
		if (!page) {
		    printf("ERROR: vm_page_t page is NULL");
		    continue;
		}

		vaddr_data = IDX_TO_VADDR(page->pindex, cur_entry->start, cur_entry->offset);
		error = fd_write(&vaddr_data, sizeof(vm_offset_t), desc);
		if (error != 0) {
		    printf("Error: writing vm_map_entry_info failed\n");
		    return error;
		}

		/* Never fails on amd64, check is here for futureproofing */
		vaddr = userpage_map(page->phys_addr);
		if ((void *) vaddr == NULL) {
		    printf("Mapping page failed\n");
		    /* EINVAL seems the most appropriate */
		    error = -EINVAL;
		    return error;
		}

		error = fd_write((void*) vaddr, PAGE_SIZE, desc);
		if (error != 0)
		    return error;

		userpage_unmap(vaddr);

	    }

	    /* For delta dumps, deallocate the object to collapse the chain again. */
	    if (mode == DELTA_DUMP) {
		vm_object_deallocate(objects[i]);
	    }
	}

	return 0;
}
