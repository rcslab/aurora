#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/bitstring.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/filedesc.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/md5.h>
#include <sys/mman.h>
#include <sys/module.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/shm.h>
#include <sys/signalvar.h>
#include <sys/syscallsubr.h>
#include <sys/taskqueue.h>
#include <sys/time.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h>
#include <vm/vm_radix.h>

#include <machine/param.h>
#include <machine/reg.h>
#include <machine/vmparam.h>

#include <slos.h>
#include <slos_inode.h>
#include <slos_io.h>
#include <sls_data.h>
#include <slsfs.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_io.h"
#include "sls_kv.h"
#include "sls_pager.h"
#include "sls_partition.h"
#include "sls_prefault.h"
#include "sls_table.h"
#include "sls_vm.h"

#define SLSTABLE_TASKWARM (256)

/* The maximum size of a single data transfer */
uint64_t sls_contig_limit = MAXBCACHEBUF;
int sls_drop_io = 0;
size_t sls_bytes_written_vfs = 0;
size_t sls_bytes_read_vfs = 0;
size_t sls_bytes_written_direct = 0;
size_t sls_bytes_read_direct = 0;
uint64_t sls_pages_grabbed = 0;
uint64_t sls_io_initiated = 0;
unsigned int sls_async_slos = 1;
static bool ssparts_imported = 0;
uint64_t sls_prefault_anonpages = 0;
uint64_t sls_prefault_anonios = 0;

uma_zone_t slstable_task_zone;

int
slstable_init(void)
{
	int error;

	slsm.slsm_tabletq = taskqueue_create("slswritetq", M_WAITOK,
	    taskqueue_thread_enqueue, &slsm.slsm_tabletq);
	if (slsm.slsm_tabletq == NULL)
		return (ENOMEM);

	error = taskqueue_start_threads(
	    &slsm.slsm_tabletq, 8, PVM, "SLOS Taskqueue Threads");
	if (error)
		return (error);

	slstable_task_zone = uma_zcreate("slstable",
	    sizeof(union slstable_taskctx), NULL, NULL, NULL, NULL,
	    UMA_ALIGNOF(union slstable_taskctx), 0);
	if (slstable_task_zone == NULL)
		return (ENOMEM);

	uma_prealloc(slstable_task_zone, SLSTABLE_TASKWARM);

	return (0);
}

void
slstable_fini(void)
{
	/* Drain the write task queue just in case. */
	if (slsm.slsm_tabletq != NULL) {
		taskqueue_drain_all(slsm.slsm_tabletq);
		taskqueue_free(slsm.slsm_tabletq);
		slsm.slsm_tabletq = NULL;
	}

	if (slstable_task_zone != NULL)
		uma_zdestroy(slstable_task_zone);
}

/* Creates an in-memory Aurora record. */
struct sls_record *
sls_getrecord(struct sbuf *sb, uint64_t slsid, uint64_t type)
{
	struct sls_record *rec;

	KASSERT(slsid != 0, ("attempting to get record with SLS ID 0"));
	KASSERT(type != 0, ("attempting to get record invalid type"));
	KASSERT(sbuf_done(sb) != 0, ("record sbuf is not done"));
	KASSERT(sbuf_len(sb) != 0, ("sbuf length 0"));
	rec = malloc(sizeof(*rec), M_SLSREC, M_WAITOK | M_ZERO);
	rec->srec_id = slsid;
	rec->srec_sb = sb;
	rec->srec_type = type;

	return (rec);
}

struct sls_record *
sls_getrecord_empty(uint64_t slsid, uint64_t type)
{
	struct sls_record *rec;
	struct sbuf *sb;

	sb = sbuf_new_auto();

	KASSERT(slsid != 0, ("attempting to get record with SLS ID 0"));
	KASSERT(type != 0, ("attempting to get record invalid type"));
	rec = malloc(sizeof(*rec), M_SLSREC, M_WAITOK | M_ZERO);
	rec->srec_id = slsid;
	rec->srec_sb = sb;
	rec->srec_type = type;

	return (rec);
}

/*
 * Seal the record, preventing any further changes.
 */
int
sls_record_seal(struct sls_record *rec)
{
	return (sbuf_finish(rec->srec_sb));
}

void
sls_record_destroy(struct sls_record *rec)
{
	struct slsvmobject *info;

	if (rec->srec_type == SLOSREC_VMOBJ) {
		info = (struct slsvmobject *)sbuf_data(rec->srec_sb);
		if (info->objptr != NULL)
			vm_object_deallocate(info->objptr);
	}

	sbuf_delete(rec->srec_sb);
	free(rec, M_SLSREC);
}

/*
 * Wrap a buffer into an sbuf.
 */
static int
sls_readrec_sbuf(char *buf, size_t len, struct sbuf **sbp)
{
	struct sbuf *sb;
	int error;

	/*
	 * Make an sbuf out of the created and set it to be dynamic so that it
	 * will be cleaned up with the sbuf. The total size needs to be less
	 * than the length of valid data, so have the total length be just over
	 * the length of the record.
	 */
	sb = sbuf_new(NULL, buf, len + 1, SBUF_FIXEDLEN);
	if (sb == NULL) {
		free(buf, M_SLSMM);
		return (ENOMEM);
	}
	SBUF_SETFLAG(sb, SBUF_DYNAMIC);
	SBUF_CLEARFLAG(sb, SBUF_INCLUDENUL);

	/* Adjust the length of the sbuf to include */
	sb->s_len = len;

	/* Close up the buffer. */
	error = sbuf_finish(sb);
	if (error != 0) {
		sbuf_delete(sb);
		return (error);
	}

	KASSERT((sb->s_flags & SBUF_FINISHED) == SBUF_FINISHED,
	    ("buffer not finished?"));
	KASSERT(sbuf_done(sb), ("sbuf is not done"));
	KASSERT(sbuf_len(sb) == len, ("buffer returns wrong length"));

	*sbp = sb;

	return (0);
}

/*
 * Read the data from the beginning of the node into an sbuf.
 */
static int
sls_readrec_buf(struct file *fp, size_t len, struct sbuf **sbp)
{
	struct sbuf *sb;
	char *buf;
	int error;

	/*
	 * Allocate the receiving buffer and associate it with the record.  Use
	 * the sbuf allocator so we can wrap it around an sbuf later.
	 */
	buf = SBMALLOC(len + 1, M_WAITOK);
	if (buf == NULL)
		return (ENOMEM);

	/* Read the data from the vnode. */
	error = slsio_fpread(fp, buf, len);
	if (error != 0) {
		SBFREE(buf);
		return (error);
	}

	error = sls_readrec_sbuf(buf, len, &sb);
	if (error != 0) {
		/* The buffer got freed in sls_readrec_sbuf. */
		return (error);
	}

	KASSERT(sbuf_done(sb), ("sbuf is unfinished"));
	KASSERT(sbuf_data(sb) != NULL, ("sbuf has no associated buffer"));
	*sbp = sb;

	return (0);
}

/*
 * Reads in the metadata record representing one or more SLS info structs.
 */
int
sls_readmeta(char *buf, size_t buflen, struct slskv_table *table)
{
	struct sls_record *rec, *stored_rec;
	size_t recsize, remaining;
	char *running_buf;
	int error;

	running_buf = buf;
	remaining = buflen;
	while (remaining > 0) {
		/* Create an empty record and copy over the SLSID and type. */
		stored_rec = (struct sls_record *)buf;
		rec = sls_getrecord_empty(
		    stored_rec->srec_id, stored_rec->srec_type);
		buf += sizeof(*rec);
		remaining -= sizeof(*rec);

		/* Get the size of the record. */
		recsize = *(size_t *)buf;
		buf += sizeof(recsize);
		remaining -= sizeof(recsize);

		KASSERT(recsize <= remaining,
		    ("recsize is %lx while remaining is %lx", recsize,
			remaining));
		/* Copy over the actual data. */
		error = sbuf_bcat(rec->srec_sb, buf, recsize);
		buf += recsize;
		remaining -= recsize;

		/* Add the record to the table to be parsed into info structs
		 * later. */
		sbuf_finish(rec->srec_sb);

		error = slskv_add(table, rec->srec_id, (uintptr_t)rec);
		if (error != 0) {
			DEBUG1("%s: duplicate record\n", __func__);
			sls_record_destroy(rec);
			return (error);
		}
	}

	KASSERT(remaining == 0, ("Read past the end of the buffer"));

	return (0);
}

/* Check if a VM object record is not anonymous, and if so, store it.  */
static int
sls_readdata_slos_vmobj(struct slspart *slsp, struct slskv_table *table,
    uint64_t slsid, struct sls_record *rec, vm_object_t *objp)
{
	struct slsvmobject *info;
	vm_object_t obj;
	uint64_t type;
	size_t size;
	int error;

	/* Add the record to the table to be parsed later. */
	error = slskv_add(table, slsid, (uintptr_t)rec);
	if (error != 0) {
		sls_record_destroy(rec);
		return (error);
	}

	/* If the record is not anonymous, we'll restore it later. */
	KASSERT(rec->srec_type == SLOSREC_VMOBJ,
	    ("invalid record type %lx", rec->srec_type));
	info = (struct slsvmobject *)sbuf_data(rec->srec_sb);
	type = info->type;
	size = info->size;

	if ((type != OBJT_DEFAULT) && (type != OBJT_SWAP)) {
		/* There no object to read. */
		*objp = NULL;
		return (0);
	}

	/* Allocate a new object. */
	obj = vm_pager_allocate(OBJT_SWAP, (void *)slsid, IDX_TO_OFF(size),
	    VM_PROT_DEFAULT, 0, NULL);

	/* If we don't get an object we are unloading the SLS. */
	if (obj == NULL) {
		slskv_del(table, slsid);
		return (EBUSY);
	}

	if (SLSP_PREFAULT(slsp))
		slspre_vector_empty(slsid, size, NULL);

	*objp = obj;

	return (0);
}

static int
sls_object_populate(vm_object_t object, vm_pindex_t start, vm_pindex_t end)
{
	size_t count = end - start;
	vm_pindex_t offset;
	vm_page_t m;
	int rahead, orig;
	int rv;

	for (offset = 0; offset != count; offset += (1 + rahead)) {
		m = vm_page_grab(object, start + offset, VM_ALLOC_NORMAL);
		if (m == NULL)
			panic("No page");

		KASSERT(
		    m->valid == 0, ("page has valid bitfield %d", m->valid));

		rahead = count - offset - 1;
		orig = rahead;
		rv = vm_pager_get_pages(object, &m, 1, NULL, &rahead);

		vm_page_xunbusy(m);

		if (rv != VM_PAGER_OK) {
			DEBUG1("Pager failed to page in with %d\n", rv);
			return (EIO);
		}
	}

	return (0);
}

/* Read data from the SLOS into a VM object. */
static int
sls_readpages_slos(struct vnode *vp, vm_object_t obj, struct slos_extent sxt,
    vm_pindex_t pindex)
{
	vm_page_t msucc;
	struct buf *bp;
	vm_page_t *ma;
	size_t pgleft;
	int error, i;
	int npages;
	bool retry;

	ma = malloc(sizeof(*ma) * btoc(MAXPHYS), M_SLSMM, M_WAITOK);

	VM_OBJECT_WLOCK(obj);

	/*
	 * Some of the pages we are trying to read might already be in the
	 * process of being paged in. Make all necessary IOs to bring in any
	 * pages not currently being paged in.
	 */
	for (pgleft = sxt.sxt_cnt; pgleft > 0; pgleft -= npages) {
		msucc = vm_page_find_least(obj, pindex);
		npages = (msucc != NULL) ? msucc->pindex - pindex : pgleft;
		if (npages == 0) {
			pindex += 1;
			continue;
		}
		npages = imin(pgleft, npages);

		bp = sls_pager_readbuf(obj, pindex, npages, &retry);
		while (retry) {
			/*
			 * If out of buffers wait until
			 * more are available.
			 */
			VM_OBJECT_WUNLOCK(obj);
			pause_sbt("wswbuf", SBT_1MS, 0, 0);
			VM_OBJECT_WLOCK(obj);
			bp = sls_pager_readbuf(obj, pindex, npages, &retry);
		}

		/*
		 * The pages to be paged in are returned busied from the SLOS.
		 * This is because the task is created from the pager, which
		 * expects the pages to be returned busied. The SLS manually
		 * unbusies them.
		 */
		memcpy(ma, bp->b_pages, sizeof(*ma) * npages);
		VM_OBJECT_WUNLOCK(obj);
		error = slos_iotask_create(vp, bp, true);
		if (error != 0) {
			free(ma, M_SLSMM);
			return (error);
		}
		VM_OBJECT_WLOCK(obj);

		for (i = 0; i < npages; i++) {
			KASSERT(ma[i]->object == obj,
			    ("page belongs to wrong"
			     "object"));
			vm_page_xunbusy(ma[i]);
		}

		pindex += npages;
	}

	/* Update the pages read counter. */
	sls_bytes_read_direct += bp->b_resid;
	VM_OBJECT_WUNLOCK(obj);

	free(ma, M_SLSMM);

	return (0);
}

static int
sls_get_extents(
    struct vnode *vp, uint64_t *numextentsp, struct slos_extent **extentsp)
{
	struct thread *td = curthread;
	struct slos_extent *extents;
	uint64_t numextents;
	int error;

	numextents = SLOS_OBJOFF;
	/* Find the extent starting with the offset provided. */
	error = VOP_IOCTL(vp, SLS_NUM_EXTENTS, &numextents, 0, NULL, td);
	if (error != 0) {
		SLS_DBG("extent seek failed with %d\n", error);
		return (error);
	}

	extents = malloc(sizeof(*extents) * numextents, M_SLSMM, M_WAITOK);
	if (extents == NULL)
		return (ENOMEM);

	/* The first extent holds the offset we should start from. */
	extents[0].sxt_lblkno = SLOS_OBJOFF;

	/* Find the extent starting with the offset provided. */
	error = VOP_IOCTL(vp, SLS_GET_EXTENTS, extents, 0, NULL, td);
	if (error != 0) {
		SLS_DBG("extent seek failed with %d\n", error);
		free(extents, M_SLSMM);
		return (error);
	}

	*extentsp = extents;
	*numextentsp = numextents;

	return (0);
}

static int
sls_readdata_slos(struct vnode *vp, vm_object_t obj)
{
	struct slos_extent *extents;
	uint64_t numextents;
	vm_pindex_t pindex;
	int error;
	int i;

	/* Get all logically and physically contiguous regions. */
	error = sls_get_extents(vp, &numextents, &extents);
	if (error != 0)
		return (error);

	for (i = 0; i < numextents; i++) {
		/* Otherwise get the VM object pages for the data. */
		pindex = extents[i].sxt_lblkno - SLOS_OBJOFF;
		error = sls_readpages_slos(vp, obj, extents[i], pindex);
		if (error != 0) {
			free(extents, M_SLSMM);
			return (error);
		}
	}

	free(extents, M_SLSMM);

	return (0);
}

int
sls_readdata_prefault(
    struct vnode *vp, vm_object_t obj, struct sls_prefault *slspre)
{
	vm_pindex_t start, offset, pindex;
	struct slos_extent *extents;
	struct slos_extent sxt;
	uint64_t numextents;
	size_t count;
	int error;
	int i;

	ASSERT_VOP_LOCKED(vp, ("prefaulting with unlocked backing vnode"));

	DEBUG1("Prefaulting object %lx", obj->objid);

	/* Get all logically and physically contiguous regions. */
	error = sls_get_extents(vp, &numextents, &extents);
	if (error != 0)
		return (error);

	offset = 0;
	for (;;) {
		/* Go over the blocks we don't need. */
		while (offset < obj->size) {
			if (bit_test(slspre->pre_map, offset) != 0)
				break;
			offset += 1;
		}

		/* Find the extent the starting set block is in. */
		for (i = 0; i < numextents; i++) {
			/* Anonymous objects have their data offset in the
			 * inode. */
			KASSERT(extents[i].sxt_lblkno >= SLOS_OBJOFF,
			    ("pindex underflow"));
			pindex = extents[i].sxt_lblkno - SLOS_OBJOFF;

			/* If the end of the extent is after the offset, we
			 * found it. */
			if (pindex + extents[i].sxt_cnt >= offset)
				break;

			sxt = extents[i];
		}

		/* All extents are before the offset. */
		if (i == numextents) {
			free(extents, M_SLSMM);
			return (0);
		}

		KASSERT(pindex <= offset,
		    ("page %ld outside of extent %ld", pindex, offset));

		sxt = extents[i];

		start = offset;
		count = 0;
		while (offset < obj->size) {
			if (bit_test(slspre->pre_map, offset) == 0)
				break;
			count += 1;
			offset += 1;

			if (count >= sxt.sxt_cnt - (start - pindex))
				break;

			if (count >= (sls_contig_limit / PAGE_SIZE))
				break;
		}

		if (start == obj->size)
			break;

		KASSERT(count > 0, ("empty extent"));

		sxt.sxt_lblkno = start + SLOS_OBJOFF;
		sxt.sxt_cnt = count;

		/* Otherwise get the VM object pages for the data. */
		error = sls_readpages_slos(vp, obj, sxt, start);
		if (error != 0) {
			free(extents, M_SLSMM);
			return (error);
		}

		atomic_add_64(&sls_prefault_anonpages, count);
		atomic_add_64(&sls_prefault_anonios, 1);
	}

	free(extents, M_SLSMM);

	return (0);
}

/*
 * Reads in a sparse record representing a VM object,
 * and stores it as a linked list of page runs.
 */
static int
sls_readdata(struct slspart *slsp, struct file *fp, uint64_t slsid,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	struct sls_prefault *slspre;
	struct sls_record *rec;
	vm_object_t obj = NULL;
	struct sbuf *sb;
	int error;

	/*
	 * Read the object from the SLOS. Seeks for SLOS nodes are block-sized,
	 * so just read the whole first block of the node.
	 */
	error = sls_readrec_buf(fp, sizeof(struct slsvmobject), &sb);
	if (error != 0) {
		DEBUG2(
		    "%s: reading the record failed with %d\n", __func__, error);
		return (error);
	}

	rec = sls_getrecord(sb, slsid, SLOSREC_VMOBJ);
	KASSERT(rec != NULL, ("no record allocated"));

	/* Create a new object if we did not already find it. */
	if (slskv_find(objtable, slsid, (uintptr_t *)&obj) == 0) {
		KASSERT(obj->objid == slsid, ("new object's objid is wrong"));
		sls_record_destroy(rec);

		error = sls_pager_obj_init(obj);
		if (error != 0)
			return (error);

	} else {
		/* Store the record for later and possibly make a new object. */
		error = sls_readdata_slos_vmobj(
		    slsp, rectable, slsid, rec, &obj);
		if (error != 0) {
			sls_record_destroy(rec);
			return (error);
		}

		/* The object is NULL for non-anonymous objects. */
		if (obj == NULL)
			return (0);

		KASSERT(obj->objid == slsid, ("new object's objid is wrong"));
		/* Add the object to the table. */
		error = slskv_add(objtable, slsid, (uintptr_t)obj);
		if (error != 0) {
			DEBUG1("%s: duplicate object\n", __func__);
			goto error;
		}
	}

	KASSERT(obj != NULL, ("no object found"));
	KASSERT(obj->type = OBJT_SWAP && (obj->flags & OBJ_AURORA),
	    ("uninitialized object of type %d", obj->type));

	if (SLSP_PREFAULT(slsp) || SLSP_DELTAREST(slsp)) {
		/*
		 * Even we have no prefault vector or can't prefault
		 * pages in, we can keep going since we have created
		 * the object.
		 */
		error = slskv_find(
		    slsm.slsm_prefault, slsid, (uintptr_t *)&slspre);
		if (error != 0)
			return (0);

		KASSERT(obj->objid == slsid, ("New object's objid is wrong"));
		sls_readdata_prefault(fp->f_vnode, obj, slspre);
		if (error != 0)
			goto error;

	} else if (SLSP_LAZYREST(slsp) == 0) {
		error = sls_readdata_slos(fp->f_vnode, obj);
		if (error != 0)
			goto error;
	}

	return (error);

error:
	VM_OBJECT_WUNLOCK(obj);
	vm_object_deallocate(obj);
	sls_record_destroy(rec);

	return (error);
}

void
sls_free_rectable(struct slskv_table *rectable)
{
	struct sls_record *rec;
	uint64_t slsid;

	if (rectable == NULL)
		return;

	KV_FOREACH_POP(rectable, slsid, rec)
	sls_record_destroy(rec);

	slskv_destroy(rectable);
}

/*
 * Read the manifest for the record. The manifest is a zero terminated
 * array of SLSIDs for VM object records, concatenated with an array
 * of concatenated serialized metadata records. This function only
 * reads the raw data and passes it on for parsing.
 */
static int
sls_read_slos_manifest(uint64_t oid, char **bufp, size_t *buflenp)
{
	struct thread *td = curthread;
	struct slos_rstat st;
	struct file *fp;
	int error;
	char *buf;

	error = slsio_open_sls(oid, false, &fp);
	if (error != 0)
		return (error);

	error = fo_ioctl(fp, SLS_GET_RSTAT, &st, td->td_ucred, td);
	if (error != 0) {
		DEBUG1("setting record type failed with %d\n", error);
		fdrop(fp, td);
		return (error);
	}

	buf = malloc(st.len, M_SLSMM, M_WAITOK);
	error = slsio_fpread(fp, buf, st.len);
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}

	/* Externalize the results. */
	*bufp = buf;
	*buflenp = st.len;

	fdrop(fp, td);

	return (0);
}

static int
sls_read_slos_datarec(struct slspart *slsp, uint64_t oid,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	struct thread *td = curthread;
	struct slos_rstat st;
	struct file *fp;
	int error, ret;

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(oid, false, &fp);
	if (error != 0)
		return (error);

	error = fo_ioctl(fp, SLS_GET_RSTAT, &st, td->td_ucred, td);
	if (error != 0) {
		DEBUG1("getting record type failed with %d\n", error);
		fdrop(fp, td);
		return (error);
	}

	/* Assert this is only for data. Metadata is extracted from the
	 * manifest. */
	KASSERT(sls_isdata(st.type), ("Reading non-data record %ld", st.type));

	ret = sls_readdata(slsp, fp, oid, rectable, objtable);

	fdrop(fp, td);

	return (ret);
}

static void
slstable_readtask(void *ctx, int __unused pending)
{
	union slstable_taskctx *taskctx = (union slstable_taskctx *)ctx;
	struct slstable_readctx *readctx = &taskctx->read;
	int error;

	/*
	 * VM object records are special, since we need to dump
	 * actual memory along with the metadata.
	 */
	error = sls_read_slos_datarec(
	    readctx->slsp, readctx->oid, readctx->rectable, readctx->objtable);
	/*
	 * Just one error is enough to stop execution, so don't worry
	 * about overwriting existing errors.
	 */
	if (error != 0)
		atomic_set_int(readctx->error, 1);

	uma_zfree(slstable_task_zone, taskctx);
}

static int
sls_read_slos_datarec_all(struct slspart *slsp, char **bufp, size_t *buflenp,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	size_t original_buflen, data_buflen, data_idlen;
	union slstable_taskctx *taskctx;
	struct slstable_readctx *readctx;
	uint64_t *data_ids;
	int error = 0;
	char *buf;
	int i;

	original_buflen = *buflenp;
	buf = *bufp;

	/*
	 * Treat the buffer as an array of OIDs. The array is preceded by
	 * its length which we read first. We then know how much to
	 * read to grab the whole array.
	 */
	data_idlen = *(uint64_t *)buf;
	buf += sizeof(data_idlen);
	original_buflen -= sizeof(data_idlen);

	/* Now that we have the length, we can read the array itself. */
	data_ids = (uint64_t *)buf;
	data_buflen = data_idlen * sizeof(*data_ids);
	KASSERT(data_buflen < original_buflen,
	    ("VM data array larger than the buffer itself"));

	for (i = 0; i < data_idlen; i++) {
		/* Create a new task. */
		taskctx = uma_zalloc(slstable_task_zone, M_WAITOK);
		readctx = &taskctx->read;
		readctx->slsp = slsp;
		readctx->oid = data_ids[i];
		readctx->objtable = objtable;
		readctx->rectable = rectable;
		readctx->error = &error;
		TASK_INIT(&readctx->tk, 0, &slstable_readtask, &readctx->tk);
		taskqueue_enqueue(slsm.slsm_tabletq, &readctx->tk);
	}

	taskqueue_drain_all(slsm.slsm_tabletq);

	if (error != 0)
		return (error);

	/* Include the terminating zero in the calculation.*/
	*bufp = &buf[data_buflen];
	*buflenp = original_buflen - data_buflen;

	return (0);
}

/* Reads in a record from the SLOS and saves it in the record table. */
int
sls_read_slos(struct slspart *slsp, struct slsckpt_data **sckptp,
    struct slskv_table *objtable)
{
	struct slsckpt_data *sckpt;
	char *input_buf, *buf;
	size_t buflen;
	int error;

	error = slsckpt_alloc(slsp, &sckpt);
	if (error != 0)
		return (error);

	SDT_PROBE1(sls, , sls_rest, , "Allocating checkpoint");

	/* Read the manifest, get the record number for the checkpoint. */
	error = sls_read_slos_manifest(slsp->slsp_oid, &buf, &buflen);
	if (error != 0) {
		DEBUG1("%s: reading the manifest failed\n", __func__);
		slsckpt_drop(sckpt);
		return (error);
	}

	input_buf = buf;

	SDT_PROBE1(sls, , sls_rest, , "Getting manifest");

	/* Extract the list of VM records, return the array of metadata. */
	error = sls_read_slos_datarec_all(
	    slsp, &buf, &buflen, sckpt->sckpt_rectable, objtable);
	if (error != 0) {
		DEBUG1("%s: reading the data records failed\n", __func__);
		goto error;
	}

	SDT_PROBE1(sls, , sls_rest, , "Getting data");

	/* Extract all metadata records. */
	error = sls_readmeta(buf, buflen, sckpt->sckpt_rectable);
	if (error != 0) {
		DEBUG1("%s: reading the metadata failed\n", __func__);
		goto error;
	}

	SDT_PROBE1(sls, , sls_rest, , "Getting metadata");

	free(input_buf, M_SLSMM);

	taskqueue_drain_all(slsm.slsm_tabletq);
	taskqueue_drain_all(slos.slos_tq);

	SDT_PROBE1(sls, , sls_rest, , "Draining the taskqueues");
	*sckptp = sckpt;

	return (0);

error:
	free(input_buf, M_SLSMM);
	slsckpt_drop(sckpt);

	return (error);
}

/*
 * Get the pages of an object in the form of a
 * linked list of contiguous memory areas.
 *
 * This function is not inlined in order to be able to use DTrace on it.
 */
static int __attribute__((noinline))
sls_writeobj_data(struct vnode *vp, vm_object_t obj, size_t offset)
{
	vm_pindex_t pindex;
	struct buf *bp;
	bool retry;
	int error;

	VM_OBJECT_ASSERT_WLOCKED(obj);

	pindex = 0;
	for (;;) {
		/*
		 * Split the object into contiguous chunks, send it to the disk.
		 * Every logically contiguous chunk is physically contiguous in
		 * backing storage.
		 */
		bp = sls_pager_writebuf(obj, pindex, sls_contig_limit, &retry);
		while (retry) {
			/*
			 * XXX hack right now, if we are out of buffers we
			 * are in deep trouble anyway.
			 */
			VM_OBJECT_WUNLOCK(obj);
			pause_sbt("wswbuf", 10 * SBT_1MS, 0, 0);
			VM_OBJECT_WLOCK(obj);
			bp = sls_pager_writebuf(
			    obj, pindex, sls_contig_limit, &retry);
		}

		VM_OBJECT_WUNLOCK(obj);
		if (bp == NULL) {
			KASSERT(retry == false, ("out of buffers"));
			break;
		}

		/*
		 * The pindex from which we're going to search for the next run
		 * of pages.
		 */
		pindex = bp->b_pages[bp->b_npages - 1]->pindex + 1;

		/*
		 * Apply an offset to the IO, useful for adding multiple
		 * small data records in the same file.
		 */
		bp->b_lblkno += offset;

		/* Update the counter. */
		sls_bytes_written_direct += bp->b_resid;

		BUF_ASSERT_LOCKED(bp);
		error = slos_iotask_create(vp, bp, sls_async_slos);
		VM_OBJECT_WLOCK(obj);
		if (error != 0) {
			return (error);
		}
	}

	VM_OBJECT_WLOCK(obj);

	return (0);
}

/*
 * Creates a record in the SLOS with the metadata held in the sbuf.
 * The record is contiguous, and only has data in the beginning.
 */
static int __attribute__((noinline)) sls_writemeta_slos(
    struct sls_record *rec, struct file **fpp, bool overwrite, uint64_t offset)
{
	struct sbuf *sb = rec->srec_sb;
	struct thread *td = curthread;
	struct slos_rstat st;
	struct file *fp;
	void *record;
	size_t len;
	int error;

	record = sbuf_data(sb);
	if (record == NULL)
		return (EINVAL);

	len = sbuf_len(sb);

	error = slsio_open_sls(rec->srec_id, true, &fp);
	if (error != 0)
		return (error);

	st.type = rec->srec_type;
	st.len = offset + len;

	KASSERT(st.type != 0, ("invalid record type"));
	KASSERT(st.len > 0, ("Writing out empty record of type %ld", st.type));

	error = fo_ioctl(fp, SLS_SET_RSTAT, &st, td->td_ucred, td);
	if (error != 0) {
		DEBUG1("setting record type failed with %d\n", error);
		return (error);
	}

	/* Keep reading until we get all the info. */
	error = slsio_fpwrite(fp, record, len);
	if (error != 0)
		goto error;

	/* Pass the open vnode to the caller if needed. */
	if (fpp != NULL) {
		*fpp = fp;
		return (0);
	}

	fdrop(fp, td);
	return (0);

error:
	if (fp != NULL)
		fdrop(fp, td);

	if (fpp != NULL)
		*fpp = NULL;

	return (error);
}

/*
 * Creates a record in the SLOS with metadata and data.
 * The metadata is typically held in the beginning of the
 * record, with the data spanning the rest of the record's
 * address space. The data is stored in a sparse manner,
 * their position in the record encoding their position
 * in the VM object.
 */
static int
sls_writedata_slos(struct sls_record *rec, struct slsckpt_data *sckpt)
{
	size_t amplification = sckpt->sckpt_attr.attr_amplification;
	struct sbuf *sb = rec->srec_sb;
	struct thread *td = curthread;
	struct slsvmobject *vminfo;
	struct file *fp, **infp;
	int error, ret = 0;
	vm_object_t obj;
	size_t offset;
	size_t i;
	if (rec->srec_type != SLOSREC_VMOBJ)
		panic("invalid type %lx for metadata", rec->srec_type);

	KASSERT(amplification > 0, ("amplification is 0"));

	/* Get the object itself, clean up the pointer when writing out. */
	vminfo = (struct slsvmobject *)sbuf_data(sb);
	obj = (vm_object_t)vminfo->objptr;
	vminfo->objptr = NULL;

	/*
	 * Send out the object's metadata. The amplification factor
	 * is used when testing to stress the store quickly simulate
	 * the creation of multiple checkpoints at once.
	 */
	for (i = 0; i < amplification; i++) {
		offset = i * sbuf_len(rec->srec_sb);
		/* Only the last iteration returns the locked vnode. */
		infp = (i == amplification - 1) ? &fp : NULL;
		error = sls_writemeta_slos(rec, infp, false, offset);
		if (error != 0)
			return (error);
	}

	/*
	 * The ID of the info struct and the in-memory pointer
	 * are identical at checkpoint time, so we use it to
	 * retrieve the object and grab its data.
	 */
	if (obj == NULL || !OBJT_ISANONYMOUS(obj))
		goto out;

	VM_OBJECT_WLOCK(obj);

	/*
	 * Send out the object's data, possibly amplify (explained above).
	 */
	for (i = 0; i < amplification; i++) {
		offset = i * obj->size * SLOS_OBJOFF;
		ret = sls_writeobj_data(fp->f_vnode, obj, offset);
		if (ret != 0)
			break;
	}

	VM_OBJECT_WUNLOCK(obj);

	/* Populate the prefault vector if we are doing delta restores. */
	if (SLSATTR_ISDELTAREST(sckpt->sckpt_attr))
		slspre_vector_populated(obj->objid, obj);

	/* Release the reference this function holds. */
	vm_object_deallocate(obj);
out:

	fdrop(fp, td);

	return (ret);
}

static void
slstable_writetask(void *ctx, int __unused pending)
{
	union slstable_taskctx *taskctx = (union slstable_taskctx *)ctx;
	struct slstable_writectx *writectx = &taskctx->write;
	int error = 0;

	error = sls_writedata_slos(writectx->rec, writectx->sckpt);
	if (error != 0)
		atomic_set_int(writectx->error, 1);

	uma_zfree(slstable_task_zone, taskctx);
}

static int __attribute__((noinline))
sls_write_slos_manifest(uint64_t oid, struct sbuf *sb)
{
	struct sls_record rec;
	int error;

	error = sbuf_finish(sb);
	if (error != 0)
		return (error);

	/* Write out the manifest to the vnode. */
	rec = (struct sls_record) {
		.srec_id = oid,
		.srec_type = SLOSREC_MANIFEST,
		.srec_sb = sb,
	};

	error = sls_writemeta_slos(&rec, NULL, true, 0);
	if (error != 0)
		return (error);

	return (0);
}

int
sls_write_slos_dataregion(struct slsckpt_data *sckpt_data)
{
	struct sls_record *rec;
	struct slskv_iter iter;
	uint64_t slsid;
	int error;

	KV_FOREACH(sckpt_data->sckpt_rectable, iter, slsid, rec)
	{
		/* We only dump VM objects. */
		KASSERT(sls_isdata(rec->srec_type),
		    ("dumping non object %ld", rec->srec_type));
		error = sls_writedata_slos(rec, sckpt_data);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}
	}

	return (0);
}

/*
 * Dumps a table of records into the SLOS. The records of VM objects
 * are special cases, since they are sparse and hold all pages that
 * need to be sent to the SLOS.
 */
int
sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt)
{
	union slstable_taskctx *taskctx;
	struct slstable_writectx *writectx;
	struct sbuf *sb_manifest;
	struct sls_record *rec;
	struct slskv_iter iter;
	uint64_t numids;
	uint64_t slsid;
	int error;

	/* Create a record for metadata, data, and the manifest itself. */
	sb_manifest = sbuf_new_auto();

	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{
		/* Create each record in parallel. */
		if (!sls_isdata(rec->srec_type))
			continue;

		taskctx = uma_zalloc(slstable_task_zone, M_WAITOK);
		writectx = &taskctx->write;
		writectx->rec = rec;
		writectx->sckpt = sckpt;
		TASK_INIT(&writectx->tk, 0, &slstable_writetask, &writectx->tk);
		taskqueue_enqueue(slsm.slsm_tabletq, &writectx->tk);
	}

	/*
	 * Prepend the data array with its size.
	 */
	numids = sbuf_len(sckpt->sckpt_dataid) / sizeof(uint64_t);
	error = sbuf_bcat(sb_manifest, &numids, sizeof(numids));
	if (error != 0)
		goto out;

	error = sbuf_bcat(sb_manifest, sbuf_data(sckpt->sckpt_dataid),
	    sbuf_len(sckpt->sckpt_dataid));
	if (error != 0)
		goto out;

	error = sbuf_bcat(sb_manifest, sbuf_data(sckpt->sckpt_meta),
	    sbuf_len(sckpt->sckpt_meta));
	if (error != 0)
		goto out;

	taskqueue_drain_all(slsm.slsm_tabletq);

	/*
	 * Write the huge metadata block.
	 */
	error = sls_write_slos_manifest(oid, sb_manifest);

out:
	sbuf_delete(sb_manifest);
	taskqueue_drain_all(slsm.slsm_tabletq);

	return (error);
}

int
sls_export_ssparts(void)
{
	size_t ssparts_len = sizeof(ssparts[0]) * SLS_OIDRANGE;
	struct thread *td = curthread;
	struct file *fp;
	int error;

	if (!ssparts_imported)
		return (0);

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(SLOS_SLSPART_INODE, true, &fp);
	if (error != 0)
		return (error);

	error = slsio_fpwrite(fp, ssparts, ssparts_len);
	DEBUG1("Wrote %ld bytes for partitions\n", ssparts_len);

	fdrop(fp, td);
	return (error);
}

int
sls_import_ssparts(void)
{
	size_t ssparts_len = sizeof(ssparts[0]) * SLS_OIDRANGE;
	struct thread *td = curthread;
	struct file *fp;
	int error;

	DEBUG1("[SSPART] Reading %ld bytes for partitions\n", ssparts_len);

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(SLOS_SLSPART_INODE, false, &fp);
	if (error != 0) {
		/*
		 * There were no partitions to speak of, because
		 * this is the first time we are mounting this SLOS.
		 */
		DEBUG("[SSPART] No partitions found\n");
		ssparts_imported = true;
		return (0);
	}

	error = slsio_fpread(fp, ssparts, ssparts_len);
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}

	if (error == 0)
		ssparts_imported = true;
	DEBUG("[SSPART] Done reading partitions\n");

	fdrop(fp, td);

	return (error);
}

static int
slspre_serialize_vector(
    struct sbuf *sb, uint64_t objid, struct sls_prefault *slspre)
{
	uint64_t objsize;
	int error;

	error = sbuf_bcat(sb, &objid, sizeof(objid));
	if (error != 0)
		return (error);

	/* Find a way to get bitmap sizes. */
	objsize = slspre->pre_size;
	error = sbuf_bcat(sb, &objsize, sizeof(objsize));
	if (error != 0)
		return (error);

	error = sbuf_bcat(sb, slspre->pre_map, bitstr_size(objsize));
	if (error != 0)
		return (error);

	return (0);
}

static int
slspre_serialize_table(struct sbuf **sbp)
{
	struct sls_prefault *slspre;
	struct sbuf *sb;
	uint64_t objid;
	int error;

	sb = sbuf_new_auto();

	KV_FOREACH_POP(slsm.slsm_prefault, objid, slspre)
	{
		error = slspre_serialize_vector(sb, objid, slspre);
		slspre_destroy(slspre);

		if (error != 0) {
			sbuf_delete(sb);
			return (error);
		}
	}

	error = sbuf_finish(sb);
	if (error != 0) {
		sbuf_delete(sb);
		return (error);
	}

	*sbp = sb;

	return (0);
}

int
slspre_export(void)
{
	struct thread *td = curthread;
	struct sbuf *sb;
	struct file *fp;
	size_t iosize;
	int error;

	if (slos.slsfs_mount == NULL)
		return (0);

	error = slspre_serialize_table(&sb);
	if (error != 0)
		return (error);

	iosize = sbuf_len(sb);

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(SLOS_SLSPREFAULT_INODE, true, &fp);
	if (error != 0) {
		sbuf_delete(sb);
		return (error);
	}

	error = slsio_fpwrite(fp, &iosize, sizeof(iosize));
	if (error != 0)
		goto done;

	error = slsio_fpwrite(fp, sbuf_data(sb), iosize);
	if (error != 0)
		goto done;

	DEBUG1("[SLSPRE] Wrote %ld bytes\n", iosize);

done:
	sbuf_delete(sb);

	fdrop(fp, td);
	return (error);
}

int
slspre_import(void)
{
	struct thread *td = curthread;
	struct sls_prefault *slspre;
	struct file *fp;
	uint64_t objid;
	size_t bitlen;
	size_t size;
	int error;

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(SLOS_SLSPREFAULT_INODE, false, &fp);
	if (error != 0) {
		/*
		 * There is no inode for prefault vectors if the SLOS
		 * is brand new. In that case we just return without
		 * needing to import anything.
		 */
		return (0);
	}

	error = slsio_fpread(fp, &size, sizeof(size));
	if (error != 0)
		goto done;

	DEBUG1("[SLSPRE] Reading %ld bytes\n", size);
	while (size > 0) {
		error = slsio_fpread(fp, &objid, sizeof(objid));
		if (error != 0)
			break;

		error = slsio_fpread(fp, &bitlen, sizeof(bitlen));
		if (error != 0)
			break;

		/* Create the prefault vector and add it to the table. */
		error = slspre_vector_empty(objid, bitlen, &slspre);
		if (error != 0)
			break;

		/* Read it in. */
		error = slsio_fpread(fp, slspre->pre_map, bitstr_size(bitlen));
		if (error != 0)
			break;

		size -= (sizeof(objid) + sizeof(bitlen) + bitstr_size(bitlen));
	}
	DEBUG("[SLSPRE] Done reading prefault vectors\n");

done:
	fdrop(fp, td);
	return (error);
}
