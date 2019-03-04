#include "_slsmm.h"
#include "cpuckpt.h"
#include "memckpt.h"
#include "slsmm.h"
#include "backends/fileio.h"

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


/*
 * These are global state for now. They will stop being so
 * if we ever do multithreaded restores.
 */

/* XXX Wrong name, it's a list and not a tailq */
struct page_tailq *slspages;
u_long hashmask;

int 
setup_hashtable(void)
{
	slspages = hashinit(HASH_MAX, M_SLSMM, &hashmask);
	if (slspages == NULL)
		return ENOMEM;

	return 0;
}


void
cleanup_hashtable(void)
{
	int i;
	struct dump_page *cur_page;
	struct page_tailq *cur_bucket;

	for (i = 0; i <= hashmask; i++) {
		cur_bucket = &slspages[i];
		while (!LIST_EMPTY(cur_bucket)) {
			cur_page = LIST_FIRST(cur_bucket);
			LIST_REMOVE(cur_page, next);
			free(cur_page->data, M_SLSMM);
			free(cur_page, M_SLSMM);
		}
	}

	hashdestroy(slspages, M_SLSMM, hashmask);
}

void 
print_bucket(int bucketnum)
{
	struct dump_page *entry;

	LIST_FOREACH(entry, &slspages[bucketnum & hashmask], next)
		printf("(Bucket %04lu) Address %lu\n", bucketnum & hashmask, entry->vaddr);
}


void 
print_list(void)
{
	int i;

	for (i = 0; i <= hashmask; i++)
		print_bucket(i);

}
