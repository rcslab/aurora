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

#include <machine/reg.h>

#include <vm/vm.h>
#include <vm/pmap.h>
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
#include "sls_process.h"

struct slsp_tailq sls_procs;

int 
slsp_init_htable(struct sls_process *slsp)
{
	slsp->slsp_pages = hashinit(HASH_MAX, M_SLSMM, &slsp->slsp_hashmask);
	if (slsp->slsp_pages == NULL)
		return ENOMEM;

	return 0;
}

void
slsp_fini_htable(struct sls_process *slsp)
{
	int i;
	struct dump_page *cur_page;
	struct page_list *cur_bucket;
	int hashmask;

	hashmask = slsp->slsp_hashmask;
	for (i = 0; i <= hashmask; i++) {
		cur_bucket = &slsp->slsp_pages[i];
		while (!LIST_EMPTY(cur_bucket)) {
			cur_page = LIST_FIRST(cur_bucket);
			LIST_REMOVE(cur_page, next);
			free(cur_page->data, M_SLSMM);
			free(cur_page, M_SLSMM);
		}
	}

	hashdestroy(slsp->slsp_pages, M_SLSMM, hashmask);
}

struct sls_process *
slsp_init(struct proc *p)
{
	struct sls_process *slsp;
	
	slsp = malloc(sizeof(*slsp), M_SLSMM, M_WAITOK);
	slsp->slsp_id = sls_metadata.slsm_lastid++;

	mtx_init(&slsp->slsp_mtx, "slspmtx", NULL, MTX_DEF);

	//slsp->slsp_proc = p;
	if (p != NULL) {
	    strncpy(slsp->slsp_name, p->p_comm, MAXCOMLEN);
	    //PHOLD(p);
	}

	slsp->slsp_dump = alloc_dump();

	slsp->slsp_pagecount = 0;
	slsp_init_htable(slsp);


	return slsp;
}

void
slsp_fini(struct sls_process *slsp)
{

	if (slsp == NULL)
	    return;

	slsp_fini_htable(slsp);

	free_dump(slsp->slsp_dump);

	/*
	if (slsp->slsp_proc != NULL)
	    PRELE(slsp->slsp_proc);
	    */

	mtx_destroy(&slsp->slsp_mtx);

	free(slsp, M_SLSMM);
}

void
slsp_list(void)
{
	struct sls_process *slsp;

	printf("|  Entry\t|    Name\t\t|    Size(KB)\t\t\n");
	TAILQ_FOREACH(slsp, &sls_procs, slsp_procs) {
	    printf("|   %d\t\t|    %s\t\t|       %ld\t\t\n", 
		    slsp->slsp_id, slsp->slsp_name, slsp->slsp_pagecount * 4);
	}
}

void
slsp_delete(int id)
{
	struct sls_process *slsp = NULL;

	TAILQ_FOREACH(slsp, &sls_procs, slsp_procs) {
	    if (slsp->slsp_id == id) {
		TAILQ_REMOVE(&sls_procs, slsp, slsp_procs);
		break;
	    }
	}

	if (slsp != NULL)
	    slsp_fini(slsp);
}

struct sls_process *
slsp_find(int id)
{
	struct sls_process *slsp;

	TAILQ_FOREACH(slsp, &sls_procs, slsp_procs) {
	    if (slsp->slsp_id == id)
		return slsp;
	}

	return NULL;
}

void
slsp_delete_all(void)
{
	struct sls_process *slsp, *prev = NULL;

	TAILQ_FOREACH(slsp, &sls_procs, slsp_procs) {
	    if (prev != NULL)
		slsp_fini(prev);
	    TAILQ_REMOVE(&sls_procs, slsp, slsp_procs);
	    prev = slsp;
	}

	if (prev != NULL)
	    slsp_fini(prev);
}

void
slsp_addpage_noreplace(struct sls_process *slsp, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;
	int already_there;
	u_long hashmask;

	vaddr = new_entry->vaddr;
	hashmask = slsp->slsp_hashmask;
	page_bucket = &slsp->slsp_pages[vaddr & hashmask];

	already_there = 0;
	LIST_FOREACH(page_entry, page_bucket, next) {
	    if(page_entry->vaddr == new_entry->vaddr) {
		free(new_entry->data, M_SLSMM);
		free(new_entry, M_SLSMM);
		already_there = 1;
		break;
	    }
	}

	if (already_there == 0) {
	    LIST_INSERT_HEAD(page_bucket, new_entry, next);
	    slsp->slsp_pagecount++;
	}
}

void
slsp_addpage_replace(struct sls_process *slsp, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;
	u_long hashmask;

	vaddr = new_entry->vaddr;
	hashmask = slsp->slsp_hashmask;
	page_bucket = &slsp->slsp_pages[vaddr & hashmask];

	LIST_FOREACH(page_entry, page_bucket, next) {
	    if(page_entry->vaddr == new_entry->vaddr) {
		LIST_REMOVE(page_entry, next);
		free(page_entry->data, M_SLSMM);
		free(page_entry, M_SLSMM);
		slsp->slsp_pagecount--;
		break;
	    }
	}

	LIST_INSERT_HEAD(page_bucket, new_entry, next);
	slsp->slsp_pagecount++;
}
