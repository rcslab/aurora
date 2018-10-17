#include "_slsmm.h"
#include "cpuckpt.h"
#include "memckpt.h"
#include "slsmm.h"
#include "fileio.h"
#include "hash.h"

#include <sys/types.h>

#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
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


struct dump *alloc_dump() {
	struct dump *dump = NULL;
	dump = malloc(sizeof(struct dump), M_SLSMM, M_NOWAIT);
	if (dump == NULL) return NULL;
	dump->threads = NULL;
	dump->entries = NULL;
	/* NOTE: The objects are _never_ used as actual objects, they
	 * are only here to pass information around and will be removed
	 * from the dump in the future. That's why we're using malloc
	 * instead of their actual constructor.
	 */
	dump->objects = NULL;
	return dump;
}

void free_dump(struct dump *dump) {
	if (!dump) return;
	if (dump->threads) free(dump->threads, M_SLSMM);
	if (dump->entries) free(dump->entries, M_SLSMM);
	if (dump->objects) free(dump->objects, M_SLSMM);
	free(dump, M_SLSMM);
}

int
load_dump(struct dump *dump, int fd)
{
	int error = 0;
	size_t size;
	void *hashpage;
	vm_offset_t vaddr;
	struct dump_page *page_entry, *new_entry; 
	struct page_tailq *page_bucket;
	int already_there;


	// load proc info
	error = fd_read(&dump->proc, sizeof(struct proc_info), fd);
	if (error) {
		printf("Error: cannot read proc_info\n");
		return error;
	}
	if (dump->proc.magic != SLS_PROC_INFO_MAGIC) {
		printf("Error: SLS_PROC_INFO_MAGIC not match\n");
		return -1;
	}

	// allocate thread info
	size = sizeof(struct thread_info) * dump->proc.nthreads;
	dump->threads = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dump->threads) {
		printf("Error: cannot allocate thread_info\n");
		return ENOMEM;
	}
	error = fd_read(dump->threads, size, fd);
	if (error) {
		printf("Error: cannot read thread_info\n");
		return error;
	}
	for (int i = 0; i < dump->proc.nthreads; i ++)
		if (dump->threads[i].magic != SLS_THREAD_INFO_MAGIC) {
			printf("Error: SLS_THREAD_INFO_MAGIC not match\n");
			return -1;
		}

	// load vmspace
	error = fd_read(&dump->vmspace, sizeof(struct vmspace_info), fd);
	if (error) {
		printf("Error: cannot read vmspace\n");
		return error;
	}
	if (dump->vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
		printf("Error: SLS_VMSPACE_INFO_MAGIC not match\n");
		return -1;
	}
	
	size = sizeof(struct vm_map_entry_info) * dump->vmspace.nentries;
	dump->entries = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dump->entries) {
		printf("Error: cannot allocate entries\n");
		return ENOMEM;
	}


	for (int i = 0; i < dump->vmspace.nentries; i++) {

		error = fd_read(&dump->entries[i], sizeof(struct vm_map_entry_info), fd);
		if (error) 
			return error;

		if (dump->entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
			printf("Error: SLS_ENTRY_INFO_MAGIC not match\n");
			return -1;
		}

		if (dump->entries[i].size == ULONG_MAX) {
			continue;
		}


		for (;;) {

			error = fd_read(&vaddr, sizeof(vm_offset_t), fd);
			if (error) 
				return error;
			if (vaddr == ULONG_MAX) break;

			/* 
			 * We assume that asking for a page-size chunk will 
			 * give us a page-aligned chunk, which makes sense given
			 * that we are probably getting it from a slab allocator.
			 */
			new_entry = malloc(sizeof(*page_entry), M_SLSMM, M_NOWAIT);
			hashpage = malloc(PAGE_SIZE, M_SLSMM, M_NOWAIT);

			/* Too lazy for proper handling (jk please remind me if this gets left in)  */
			KASSERT(hashpage & new_entry, "hashpage non-null");

			new_entry->vaddr = vaddr;
			new_entry->data =  hashpage;
			error = fd_read(hashpage, PAGE_SIZE, fd);
			/* Put some error handling here FFS (to no one in particular) */

			/* 
			 * Add the new page to the hash table, if it already  
			 * exists there don't replace it (we suppose we are
			 * calling load_dump() from the freshest dump to the 
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

	return error;
}


struct dump *
compose_dump(int nfds, int *fds)
{
	struct dump *dump = alloc_dump();
	struct dump *currdump = alloc_dump();
	int error = 0;

	if (!dump || !currdump) {
		printf("Error: cannot allocate dump struct at slsmm_restore\n");
		goto error;
	}

	/*
	 * The only dump that we actually care about, the rest 
	 */
	error = load_dump(dump, fds[nfds-1]);
	if (error) {
		printf("Error: cannot load dumps\n");
		goto error;
	}

	/*
	 * We are constantly loading dumps because we only need the side-effect
	 * of this action - populating the address space.
	 */
	for (int i = 0; i < nfds - 1; i++) {
		/* Memory leak (the thread/entry arrays), will be fixed when we flesh out alloc_dump()*/
		error = load_dump(currdump, fds[i]);

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
