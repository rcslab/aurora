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
#include "sls_snapshot.h"
#include "sls_process.h"

int 
slss_init_htable(struct sls_snapshot *slss)
{
	slss->slss_pages = hashinit(HASH_MAX, M_SLSMM, &slss->slss_hashmask);
	if (slss->slss_pages == NULL)
		return ENOMEM;

	return 0;
}

void
slss_fini_htable(struct sls_snapshot *slss)
{
	int i;
	struct dump_page *cur_page;
	struct page_list *cur_bucket;
	int hashmask;

	hashmask = slss->slss_hashmask;
	for (i = 0; i <= hashmask; i++) {
		cur_bucket = &slss->slss_pages[i];
		while (!LIST_EMPTY(cur_bucket)) {
			cur_page = LIST_FIRST(cur_bucket);
			LIST_REMOVE(cur_page, next);
			free(cur_page->data, M_SLSMM);
			free(cur_page, M_SLSMM);
		}
	}

	hashdestroy(slss->slss_pages, M_SLSMM, hashmask);
}

struct sls_snapshot *
slss_init(struct proc *p, int mode)
{
	struct sls_snapshot *slss;
	
	slss = malloc(sizeof(*slss), M_SLSMM, M_WAITOK | M_ZERO);
	slss->slss_id = slsm.slsm_lastid++;

	mtx_init(&slss->slss_mtx, "slssmtx", NULL, MTX_DEF);

	if (p != NULL)
	    strncpy(slss->slss_name, p->p_comm, MAXCOMLEN);

	slss->slss_dump = alloc_dump();

	if (p != NULL)
	    slss->slss_pid = p->p_pid;
	else 
	    slss->slss_pid = 0;
	slss->slss_pagecount = 0;
	slss->slss_mode = mode;
	slss_init_htable(slss);


	return slss;
}

void
slss_fini(struct sls_snapshot *slss)
{

	if (slss == NULL)
	    return;

	slss_fini_htable(slss);

	free_dump(slss->slss_dump);

	mtx_destroy(&slss->slss_mtx);

	free(slss, M_SLSMM);
}

void
slss_list(struct slss_list *slist)
{
	struct sls_snapshot *slss;

	printf("|  Entry\t|    Name\t\t|    Size(KB)\t\t\n");
	LIST_FOREACH(slss, slist, slss_snaps) {
	    printf("|   %d\t\t|    %s\t\t|       %ld\t\t\n", 
		    slss->slss_id, slss->slss_name, slss->slss_pagecount * 4);
	}
}

void
slss_listall(void)
{
	slss_list(&slsm.slsm_snaplist);
}

void
slss_delete(int id)
{
	struct sls_snapshot *slss = NULL;
	struct sls_process *slsp;


	LIST_FOREACH(slss, &slsm.slsm_snaplist, slss_snaps) {
	    if (slss->slss_id == id) {

		slsp = slsp_find(slss->slss_pid);
		if (slsp == NULL) {
		    printf("Warning: Found an sls_snapshot without an sls_process (PID %d).\n", slss->slss_pid);
		}

		LIST_REMOVE(slss, slss_procsnaps);
		LIST_REMOVE(slss, slss_snaps);
		break;
	    }
	}

	if (slss != NULL)
	    slss_fini(slss);
}

struct sls_snapshot *
slss_find(int id)
{
	struct sls_snapshot *slss;

	LIST_FOREACH(slss, &slsm.slsm_snaplist, slss_snaps) {
	    if (slss->slss_id == id)
		return slss;
	}

	return NULL;
}

void
slss_delete_all(void)
{
	struct sls_snapshot *slss, *prev = NULL;
	struct sls_process *slsp;

	LIST_FOREACH(slss, &slsm.slsm_snaplist, slss_snaps) {
	    if (prev != NULL) {
		printf("Removing snapshot %d\n", prev->slss_id);
		slss_fini(prev);
	    }

	    slsp = slsp_find(slss->slss_pid);
	    LIST_REMOVE(slss, slss_procsnaps);
	    LIST_REMOVE(slss, slss_snaps);

	    prev = slss;
	}

	if (prev != NULL) {
	    printf("Removing snapshot %d\n", prev->slss_id);
	    slss_fini(prev);
	}
}

void
slss_addpage_noreplace(struct sls_snapshot *slss, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;
	int already_there;
	u_long hashmask;

	vaddr = new_entry->vaddr;
	hashmask = slss->slss_hashmask;
	page_bucket = &slss->slss_pages[vaddr & hashmask];

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
	    slss->slss_pagecount++;
	}
}

void
slss_addpage_replace(struct sls_snapshot *slss, struct dump_page *new_entry)
{
	struct page_list *page_bucket;
	struct dump_page *page_entry;
	vm_offset_t vaddr;
	u_long hashmask;

	vaddr = new_entry->vaddr;
	hashmask = slss->slss_hashmask;
	page_bucket = &slss->slss_pages[vaddr & hashmask];

	LIST_FOREACH(page_entry, page_bucket, next) {
	    if(page_entry->vaddr == new_entry->vaddr) {
		LIST_REMOVE(page_entry, next);
		free(page_entry->data, M_SLSMM);
		free(page_entry, M_SLSMM);
		slss->slss_pagecount--;
		break;
	    }
	}

	LIST_INSERT_HEAD(page_bucket, new_entry, next);
	slss->slss_pagecount++;
}
