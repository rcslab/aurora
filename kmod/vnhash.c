#include "_slsmm.h"
#include "cpuckpt.h"
#include "memckpt.h"
#include "slsmm.h"
#include "backends/fileio.h"
#include "backends/fileio.h"
#include "vnhash.h"

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
struct vnentry_tailq *slsnames;
u_long vnhashmask;

int
setup_vnhash(void)
{
	slsnames = hashinit(HASH_MAX, M_SLSMM, &vnhashmask);
	if (slsnames == NULL)
		return ENOMEM;

	return 0;
}


void
cleanup_vnhash(void)
{
	int i;
	struct dump_vnentry *cur_vnentry;
	struct vnentry_tailq *cur_bucket;

	for (i = 0; i <= vnhashmask; i++) {
		cur_bucket = &slsnames[i];
		while (!LIST_EMPTY(cur_bucket)) {
			cur_vnentry = LIST_FIRST(cur_bucket);
			LIST_REMOVE(cur_vnentry, next);
			free(cur_vnentry->dump_filename->string, M_SLSMM);
			free(cur_vnentry->dump_filename, M_SLSMM);
			free(cur_vnentry, M_SLSMM);
		}
	}

	hashdestroy(slsnames, M_SLSMM, vnhashmask);
}


struct dump_filename *
vnhash_find(void *vnode)
{
	struct dump_vnentry *entry;

	LIST_FOREACH(entry, &slsnames[(u_long) vnode & vnhashmask], next)
	    if (entry->vnode == vnode)
		return entry->dump_filename;

	return NULL;
}

void
print_vnbucket(int bucketnum)
{
	struct dump_vnentry *entry;

	LIST_FOREACH(entry, &slsnames[bucketnum & vnhashmask], next)
		printf("(Bucket %04lu) Address %lu Value %s\n",
			bucketnum & vnhashmask,
			entry->dump_filename->len, entry->dump_filename->string);
}


void
print_vnlist(void)
{
	int i;

	for (i = 0; i <= vnhashmask; i++)
		print_bucket(i);

}
