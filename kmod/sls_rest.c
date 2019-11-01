#include <sys/types.h>

#include <sys/capsicum.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/syscallsubr.h>
#include <sys/time.h>
#include <sys/uio.h>


#include <machine/param.h>
#include <machine/reg.h>
#include <machine/vmparam.h>

#include <netinet/in.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "sls_data.h"
#include "sls_file.h"
#include "sls_ioctl.h"
#include "sls_kv.h"
#include "sls_load.h"
#include "sls_mm.h"
#include "sls_proc.h"
#include "sls_table.h"
#include "sls_vmobject.h"
#include "sls_vmspace.h"

#include "../slos/slos_record.h"

static int
slsrest_dothread(struct proc *p, char **bufp, size_t *buflenp)
{
	struct slsthread slsthread;
	int error;

	error = slsload_thread(&slsthread, bufp, buflenp);
	if (error != 0)
	    return error;

	error = slsrest_thread(p, &slsthread);
	if (error != 0)
	    return error;

	return 0;
}

static int
slsrest_doproc(struct proc *p, char **bufp, size_t *buflenp)
{
	struct slsproc slsproc; 
	int error, i;

	error = slsload_proc(&slsproc, bufp, buflenp);
	if (error != 0)
	    return error;

	error = slsrest_proc(p, &slsproc);
	if (error != 0)
	    return error;

	for (i = 0; i < slsproc.nthreads; i++) {
	    error = slsrest_dothread(p, bufp, buflenp);
	    if (error != 0)
		return error;
	}

	return 0;
}

struct slskqueue *slskq = NULL;
struct slsfile slsfp;

/* XXX Decouple files from processes. */
static int
slsrest_dofile(struct proc *p, int *done, char **bufp, size_t *buflenp)
{
	struct slsfile slsfile;
	void *data;
	int error;

	*done = 0;

	error = slsload_file(&slsfile, &data, bufp, buflenp);
	if (error != 0)
	    return error;

	/* XXX Factor this out, now know how many files we have. */
	if (slsfile.magic == SLSFILES_END) {
	    *done = 1;
	    return 0;
	}

	/* XXX Remove the kqueue hack. */
	if (slsfile.type != DTYPE_KQUEUE) {
	    error = slsrest_file(p, data, &slsfile);
	    if (error != 0)
		return error;
	}

	switch (slsfile.type) {
	case DTYPE_VNODE:
	    sbuf_delete((struct sbuf *) data);
	    break;

	case DTYPE_KQUEUE:
	    memcpy(&slsfp, &slsfile, sizeof(slsfp));
	    slskq = (struct slskqueue *) data;
	    break;

	case DTYPE_PIPE:
	    free(data, M_SLSMM);
	    break;

	case DTYPE_SOCKET:
	    free(data, M_SLSMM);
	    break;

	default:
	    return EINVAL;

	}

	return 0;
}

static int
slsrest_dofiledesc(struct proc *p, char **bufp, size_t *buflenp)
{
	int error = 0;
	struct slsfiledesc slsfiledesc;
	int done;

	error = slsload_filedesc(&slsfiledesc, bufp, buflenp);
	if (error != 0)
	    return error;

	error = slsrest_filedesc(p, slsfiledesc);
	sbuf_delete(slsfiledesc.cdir);
	sbuf_delete(slsfiledesc.rdir);
	if (error != 0)
	    return error;

	do {
	    error = slsrest_dofile(p, &done, bufp, buflenp);
	    if (error != 0)
		return error;
	} while (!done);

	if (slskq != NULL) {
	    error = slsrest_file(p, slskq, &slsfp);
	    free(slskq, M_SLSMM);
	    slskq = NULL;

	    if (error != 0)
		return error;

	}


	return error;
}

static int
slsrest_domemory(struct proc *p, char **bufp, size_t *buflenp, 
	struct slskv_table *objtable)
{
	struct slsvmentry slsvmentry;
	struct slsvmspace slsvmspace;
	vm_map_t map;
	int error, i;

	/* Restore the VM space and its map. */
	error = slsload_vmspace(&slsvmspace, bufp, buflenp);
	if (error != 0)
	    goto out;

	error = slsrest_vmspace(p, &slsvmspace);
	if (error != 0)
	    goto out;

	map = &p->p_vmspace->vm_map;

	/* Create the individual VM entries. */
	for (i = 0; i < slsvmspace.nentries; i++) {
	    error = slsload_vmentry(&slsvmentry, bufp, buflenp);
	    if (error != 0)
		goto out;

	    PROC_UNLOCK(p);
	    error = slsrest_vmentry(map, &slsvmentry, objtable);
	    PROC_LOCK(p);
	    if (error != 0)
		goto out;
	}
	
out:

	return error;
}

/*
 * Restore a process' local data (threads, VM map, file descriptor table).
 */
static int
slsrest_metadata(struct proc *p, struct slskv_table *proctable, 
	struct slskv_table *objtable, char **bufp, size_t *buflenp)
{
	int error;
	
	/* 
	 * Restore CPU state, file state, and memory 
	 * state, parsing the buffer at each step. 
	 */
	error = slsrest_doproc(p, bufp, buflenp);
	if (error != 0)
	    return error;

	error = slsrest_dofiledesc(p, bufp, buflenp);
	if (error != 0)
	    return error;

	error = slsrest_domemory(p, bufp, buflenp, objtable);
	if (error != 0)
	    return error;

	/* 
	 * XXX Right now we can only restore on top of
	 * _one_ process. When we are able to do multiple
	 * processes at once, we will have a reasonable
	 * key. For now, it doesn't matter.
	 */
	error = slskv_add(proctable, 0UL, (uintptr_t) p);
	if (error != 0)
	    return error;
	
	return 0;
}

/*
 * The same as vm_object_shadow, with different refcount handling and return values.
 * We also always create a shadow, regardless of the refcount.
 */
static void
slsrest_shadow(vm_object_t shadow, vm_object_t source, vm_ooffset_t offset)
{
	/*
	 * Store the offset into the source object, and fix up the offset into
	 * the new object.
	 */
	shadow->backing_object = source;
	shadow->backing_object_offset = offset;


	/* 
	* If this is the first shadow, then we transfer the reference
	* from the caller to the shadow, as done in vm_object_shadow.
	* Otherwise we add a reference to the shadow.
	*/
	if (source->shadow_count != 0)
	    source->ref_count += 1;

	VM_OBJECT_WLOCK(source);
	shadow->domain = source->domain;
	LIST_INSERT_HEAD(&source->shadow_head, shadow, shadow_list);
	source->shadow_count++;

#if VM_NRESERVLEVEL > 0
	shadow->flags |= source->flags & OBJ_COLORED;
	shadow->pg_color = (source->pg_color + OFF_TO_IDX(offset)) &
	    ((1 << (VM_NFREEORDER - 1)) - 1);
#endif

	VM_OBJECT_WUNLOCK(source);
}

static int
sls_rest(struct proc *p, struct sls_backend backend)
{
	struct slskv_table *metatable = NULL, *datatable = NULL;
	struct slskv_table *proctable = NULL, *objtable = NULL;
	struct slspagerun *pagerun, *tmppagerun;
	struct slsvmobject slsvmobject, *slsvmobjectp;
	vm_object_t parent, object;
	struct slsdata *slsdata;
	struct slskv_iter iter;
	struct slos_vnode *vp;
	struct slos_rstat *st;
	size_t buflen;
	void *record;
	char *buf;
	int error;

	/* Bring in the checkpoints from the backend. */
	
	/* ------------ SLOS-Specific Part ------------ */
	SLS_DBG("Opening inode\n");

	/* XXX Temporary until we change to multiple inodes per checkpoint. */
	vp = slos_iopen(&slos, 1024);
	if (vp == NULL)
	    return EIO; 

	SLS_DBG("Reading in data\n");

	/* Bring in the whole checkpoint in the form of SLOS records. */
	error = sls_read_slos(vp, &metatable, &datatable);
	if (error != 0) {
	    slos_iclose(&slos, vp);
	    goto out;
	}

	SLS_DBG("Closing inode\n");
	error = slos_iclose(&slos, vp); 
	if (error != 0)
	    goto out;


	/* ------------ End SLOS-Specific Part ------------ */
	SLS_DBG("Creating tables\n");

	/* Set up the restored process and VM object tables. */
	error = slskv_create(&proctable, SLSKV_NOREPLACE);
	if (error != 0)
	    goto out;

	error = slskv_create(&objtable, SLSKV_NOREPLACE);
	if (error != 0)
	    goto out;

	/*
	* XXX We don't actually need that, right? We're overwriting ourselves,
	* so we definitely don't want to stop.
	*/
	/* XXX Refactor to account for multiple processes. */
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	SLS_DBG("First pass\n");

	/* 
	 * Iterate through the metadata; each entry represents either 
	 * a process, complete with threads, FDs, and a VM map, a VM
	 * object together with its data, or (XXX later) a vnode/kqueue
	 * pipe/socket. Switching on the type, we use the appropriate
	 * restore routine.
	 *
	 * These entities are interdependent, but are restored independently.
	 * We mend the references they have to each other later in the code,
	 * but for now we have pointers between entities point to the slsid
	 *
	 * of the soon-to-be-restored object's record.
	 */

	/* 
	 * XXX Right now we do 3 passes on the table. We can avoid that if we
	 * split the objects and the processes into different kinds of tables.
	 * It's kinda messy, anyway, and we bounce among the hashtables. 
	 */

	/* First pass; create of find all objects to be used. */
	iter = slskv_iterstart(metatable);
	while (slskv_itercont(&iter, (uint64_t *) &record, (uintptr_t *) &st) != SLSKV_ITERDONE) {
	    buf = (char *) record;
	    buflen = st->len;

	    if (st->type != SLOSREC_VMOBJ)
		continue;

	    /* Get the data associated with the object in the table. */
	    error =  slsload_vmobject(&slsvmobject, &buf, &buflen);
	    if (error != 0)
		goto out;

	    error = slskv_find(datatable, (uint64_t) record, (uintptr_t *) &slsdata);
	    if (error != 0)
		goto out;

	    /* Restore the object */
	    error = slsrest_vmobject(&slsvmobject, objtable, slsdata);
	    if (error != 0)
		goto out;

	    /* XXX sb_delete() for possible sbs for the names */
	}

	SLS_DBG("Second pass\n");

	/* Second pass; link up the objects to their shadows. */
	iter = slskv_iterstart(metatable);
	while (slskv_itercont(&iter, (uint64_t *) &record, (uintptr_t *) &st) != SLSKV_ITERDONE) {
	    if (st->type != SLOSREC_VMOBJ)
		continue;

	    /* 
	     * Get the object restored for the given info struct. 
	     * We take advantage of the fact that the VM object info
	     * struct is the first thing in the record to typecast
	     * the latter into the former, skipping the parse function.
	     */
	    slsvmobjectp = (struct slsvmobject *) record; 
	    error = slskv_find(objtable, slsvmobjectp->slsid, (uintptr_t *) &object);
	    if (error != 0)
		goto out;

	    /* Try to find a parent for the restored object, if it exists. */
	    error = slskv_find(objtable, (uint64_t) slsvmobjectp->backer, (uintptr_t *) &parent);
	    if (error != 0)
		continue;
	    
	    slsrest_shadow(object, parent, slsvmobjectp->backer_off);
	}

	SLS_DBG("Third pass\n");
	/* 
	 * Third pass; restore processes. These depend on the objects 
	 * restored above, which we pass through the object table.
	 */
	iter = slskv_iterstart(metatable);
	while (slskv_itercont(&iter, (uint64_t *) &record, (uintptr_t *) &st) != SLSKV_ITERDONE) {
	    if (st->type != SLOSREC_PROC) {
		continue;
	    }

	    buf = (char *) record;
	    buflen = st->len;

	    error = slsrest_metadata(p, proctable, objtable, &buf, &buflen);
	    if (error != 0)
		goto out;

	}

	SLS_DBG("Done\n");
	kern_psignal(p, SIGCONT);

out:
	PROC_UNLOCK(p);

	/* 
	 * The process and object tables hold pointers to processes and 
	 * VM objects as values, so we don't need to free anything. 
	 */
	if (objtable != NULL)
	    slskv_destroy(objtable);

	if (proctable != NULL)
	    slskv_destroy(proctable);

	/* Cleanup the tables used for bringing the data in memory. */
	if (metatable != NULL) {
	    while (slskv_pop(metatable, (uint64_t *) &record, (uintptr_t *) &st) == 0) {
		free(st, M_SLSMM);
		free(record, M_SLSMM);
	    }

	    slskv_destroy(metatable);
	}
	
	if (datatable != NULL) {
	    while (slskv_pop(datatable, (uint64_t *) &record, (uint64_t *) &slsdata) == 0) {
		LIST_FOREACH_SAFE(pagerun, slsdata, next, tmppagerun) {
		    LIST_REMOVE(pagerun, next);
		    free(pagerun->data, M_SLSMM);
		    uma_zfree(slspagerun_zone, pagerun);
		}
		free(slsdata, M_SLSMM);
	    }

	    slskv_destroy(datatable);
	}


	SLS_DBG("error %d\n", error);

	return error;
}

void
sls_restored(struct sls_restored_args *args)
{
	int error;

	/* Restore the old process. */
	error = sls_rest(args->p, args->backend);
	if (error != 0)
	    printf("Error: sls_rest failed with %d\n", error);

	PRELE(args->p);

	/* If we restored from a file, release the sbuf with the name. */
	if (args->backend.bak_target == SLS_FILE)
	    sbuf_delete(args->backend.bak_name);
	free(args, M_SLSMM);

	/* Release the reference to the SLS device */
	dev_relthread(slsm.slsm_cdev, 1);

	/* 
	 * If something went wrong destroy the whole
	 * process, otherwise have this thread exit.
	 * The restored threads will keep on executing.
	 */
	if (error != 0)
	    exit1(curthread, error, 0);
	else
	    kthread_exit();
}
