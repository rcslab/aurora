#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/bitstring.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
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
#include "sls_kv.h"
#include "sls_pager.h"
#include "sls_partition.h"
#include "sls_prefault.h"
#include "sls_table.h"
#include "sls_vm.h"

#define SLSTABLE_TASKWARM (32)

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

static uma_zone_t slstable_task_zone;

struct slstable_taskctx {
	struct task tk;
	struct sls_record *rec;
	struct slskv_table *objtable;
};

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
	    sizeof(struct slstable_taskctx), NULL, NULL, NULL, NULL,
	    UMA_ALIGNOF(struct slstable_taskctx), 0);
	if (slstable_task_zone == NULL)
		return (ENOMEM);

	uma_prealloc(slstable_task_zone, SLSTABLE_TASKWARM);

	return (0);
}

void
slstable_fini(void)
{
	/* Drain the SLOS taskqueue, which might be doing IOs. */
	if (slos.slos_tq != NULL)
		taskqueue_drain_all(slos.slos_tq);

	/* Drain the write task queue just in case. */
	if (slsm.slsm_tabletq != NULL) {
		taskqueue_drain_all(slsm.slsm_tabletq);
		taskqueue_free(slsm.slsm_tabletq);
		slsm.slsm_tabletq = NULL;
	}

	if (slstable_task_zone != NULL)
		uma_zdestroy(slstable_task_zone);
}

static int
sls_isdata(uint64_t type)
{
	return (type == SLOSREC_VMOBJ);
}

static int
sls_doio(struct vnode *vp, void *buf, size_t len, size_t offset, enum uio_rw rw)
{
	size_t iosize = 0;
	uint64_t back = 0;
	struct iovec aiov;
	struct uio auio;
	int error = 0;

	ASSERT_VOP_LOCKED(vp, ("vnode %p is unlocked", vp));

	aiov.iov_base = buf;
	aiov.iov_len = len;
	slos_uioinit(&auio, offset, rw, &aiov, 1);
	auio.uio_resid = len;

	/* If we don't want to do anything just return. */
	if (sls_drop_io)
		return (0);

	/* Do the IO itself. */
	iosize = auio.uio_resid;

	while (auio.uio_resid > 0) {
		back = auio.uio_resid;
		if (auio.uio_rw == UIO_WRITE) {
			error = VOP_WRITE(vp, &auio, 0, NULL);
		} else {
			error = VOP_READ(vp, &auio, 0, NULL);
		}
		if (error != 0) {
			goto out;
		}
		MPASS(back != auio.uio_resid);
	}
	if (auio.uio_rw == UIO_WRITE)
		sls_bytes_written_vfs += iosize;
	else
		sls_bytes_read_vfs += iosize;
out:

	sls_io_initiated += 1;
	if (error != 0)
		SLS_ERROR(sls_doio, error);

	return (error);
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
 * Get the size and type of a SLOS vnode.
 */
static int
sls_get_rstat(struct vnode *vp, struct slos_rstat *st)
{
	struct thread *td = curthread;
	int error;

	VOP_UNLOCK(vp, 0);
	error = VOP_IOCTL(vp, SLS_GET_RSTAT, st, 0, NULL, td);
	VOP_LOCK(vp, LK_EXCLUSIVE);

	return (error);
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
sls_readrec_buf(struct vnode *vp, size_t len, struct sbuf **sbp)
{
	struct sbuf *sb;
	char *buf;
	int error;

	/*
	 * Allocate the receiving buffer and associate it with the record.  Use
	 * the sbuf allocator so we can wrap it around an sbuf later.
	 */
	buf = SBMALLOC(len + 1);
	if (buf == NULL)
		return (ENOMEM);

	/* Read the data from the vnode. */
	error = sls_doio(vp, buf, len, 0, UIO_READ);
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
static int
sls_readmeta_slos(char *buf, size_t buflen, struct slskv_table *table)
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
		slspre_vector_empty(slsid, size);

	*objp = obj;

	return (0);
}

/* Read data from the SLOS into a VM object. */
static int
sls_readpages_slos(struct vnode *vp, vm_object_t obj, struct slos_extent sxt)
{
	vm_pindex_t pindex;
	vm_page_t msucc;
	struct buf *bp;
	vm_page_t *ma;
	size_t pgleft;
	int error, i;
	int npages;
	bool retry;

	ma = malloc(sizeof(*ma) * btoc(MAXPHYS), M_SLSMM, M_WAITOK);

	KASSERT(sxt.sxt_lblkno >= SLOS_OBJOFF,
	    ("Invalid start of extent %ld", sxt.sxt_lblkno));
	KASSERT(obj->ref_count == 1, ("object under reconstruction is in use"));
	VM_OBJECT_WLOCK(obj);

	/*
	 * Some of the pages we are trying to read might already be in the
	 * process of being paged in. Make all necessary IOs to bring in any
	 * pages not currently being paged in.
	 */
	pindex = sxt.sxt_lblkno - SLOS_OBJOFF;
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
sls_readdata_slos(struct vnode *vp, vm_object_t obj)
{
	struct thread *td = curthread;
	struct slos_extent extent;
	int error;

	/* Start from the beginning of the data region. */
	extent.sxt_lblkno = SLOS_OBJOFF;
	extent.sxt_cnt = 0;
	for (;;) {
		/* Find the extent starting with the offset provided. */
		error = VOP_IOCTL(vp, SLS_SEEK_EXTENT, &extent, 0, NULL, td);
		if (error != 0) {
			SLS_DBG("extent seek failed with %d\n", error);
			goto error;
		}

		/* Get the new extent. If we get no more data, we're done. */
		if (extent.sxt_cnt == 0)
			break;

		/* Otherwise get the VM object pages for the data. */
		error = sls_readpages_slos(vp, obj, extent);
		if (error != 0)
			goto error;

		extent.sxt_lblkno += extent.sxt_cnt;
		extent.sxt_cnt = 0;
	}

	return (0);

error:
	VOP_UNLOCK(vp, 0);
	return (error);
}

static int
sls_readdata_prefault(
    struct vnode *vp, vm_object_t obj, struct sls_prefault *slspre)
{
	struct slos_extent extent;
	vm_pindex_t offset;
	int error;

	ASSERT_VOP_LOCKED(vp, ("prefaulting with unlocked backing vnode"));

	DEBUG1("Prefaulting object %lx", obj->objid);
	offset = 0;
	for (;;) {
		/* Go over the blocks we don't need */
		while (offset < obj->size) {
			if (bit_test(slspre->pre_map, offset) != 0)
				break;
			offset += 1;
		}

		extent.sxt_lblkno = offset + SLOS_OBJOFF;
		extent.sxt_cnt = 0;
		while (offset < obj->size) {
			if (bit_test(slspre->pre_map, offset) == 0)
				break;
			offset += 1;
			extent.sxt_cnt += 1;

			if (extent.sxt_cnt == (sls_contig_limit / PAGE_SIZE))
				break;
		}

		if (offset == obj->size)
			break;

		KASSERT(extent.sxt_cnt > 0, ("Empty extent"));

		error = sls_readpages_slos(vp, obj, extent);
		if (error != 0)
			return (error);
	}

	return (0);
}

/*
 * Reads in a sparse record representing a VM object,
 * and stores it as a linked list of page runs.
 */
static int
sls_readdata(struct slspart *slsp, struct vnode *vp, uint64_t slsid,
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
	error = sls_readrec_buf(vp, sizeof(struct slsvmobject), &sb);
	if (error != 0)
		return (error);

	rec = sls_getrecord(sb, slsid, SLOSREC_VMOBJ);
	KASSERT(rec != NULL, ("No record allocated"));

	/* Create a new object if we did not already find it. */
	if (slskv_find(objtable, slsid, (uintptr_t *)&obj) == 0) {
		sls_record_destroy(rec);

		error = sls_pager_obj_init(obj);
		if (error != 0)
			return (error);
	} else {
		/* Store the record for later and possibly make a new object. */
		VOP_UNLOCK(vp, 0);
		error = sls_readdata_slos_vmobj(
		    slsp, rectable, slsid, rec, &obj);
		VOP_LOCK(vp, LK_EXCLUSIVE);
		if (error != 0) {
			sls_record_destroy(rec);
			return (error);
		}

		/* The object is NULL for non-anonymous objects. */
		if (obj == NULL)
			return (0);

		/* Add the object to the table. */
		error = slskv_add(objtable, slsid, (uintptr_t)obj);
		if (error != 0)
			goto error;
	}

	KASSERT(obj != NULL, ("No object found"));

	if (SLSP_LAZYREST(slsp) == 0) {
		VOP_UNLOCK(vp, 0);
		error = sls_readdata_slos(vp, obj);
		VOP_LOCK(vp, LK_EXCLUSIVE);

		if (error != 0)
			goto error;
	} else if (SLSP_PREFAULT(slsp) || SLSP_DELTAREST(slsp)) {
		/*
		 * Even we have no prefault vector or can't prefault
		 * pages in, we can keep going since we have created
		 * the object.
		 */
		error = slskv_find(
		    slsm.slsm_prefault, slsid, (uintptr_t *)&slspre);
		if (error != 0)
			return (0);

		error = sls_readdata_prefault(vp, obj, slspre);
		if (error != 0)
			return (0);
	}


	return (0);

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
	int close_error, error;
	struct slos_rstat st;
	struct vnode *vp;
	int mode = FREAD;
	char *buf;

	/* Get the vnode for the record and open it. */
	error = VFS_VGET(slos.slsfs_mount, oid, LK_EXCLUSIVE, &vp);
	if (error != 0)
		return (error);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0) {
		vput(vp);
		return (error);
	}

	/* Get the record length. */
	error = sls_get_rstat(vp, &st);
	if (error != 0) {
		goto error;
	}

	buf = malloc(st.len, M_SLSMM, M_WAITOK);
	error = sls_doio(vp, buf, st.len, 0, UIO_READ);
	if (error != 0)
		goto error;

	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);

	/* Externalize the results. */
	*bufp = buf;
	*buflenp = st.len;

	return (0);

error:
	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);
	return (error);
}

static int
sls_read_slos_datarec(struct slspart *slsp, uint64_t oid,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	struct thread *td = curthread;
	int close_error, error, ret;
	struct slos_rstat st;
	struct vnode *vp;
	int mode = FREAD;

	/* Get the vnode for the record and open it. */
	error = VFS_VGET(slos.slsfs_mount, oid, LK_EXCLUSIVE, &vp);
	if (error != 0)
		return (error);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0) {
		vput(vp);
		return (error);
	}

	/* Get the record type. */
	error = sls_get_rstat(vp, &st);
	if (error != 0) {
		vput(vp);
		DEBUG1("getting record type failed with %d\n", error);
		return (error);
	}

	/* Assert this is only for data. Metadata is extracted from the
	 * manifest. */
	KASSERT(sls_isdata(st.type), ("Reading non-data record"));

	ret = sls_readdata(slsp, vp, oid, rectable, objtable);
	if (ret != 0)
		goto out;

out:
	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		DEBUG1("error %d, could not close slos node", close_error);

	vput(vp);

	return (ret);
}

static int
sls_read_slos_datarec_all(struct slspart *slsp, char **bufp, size_t *buflenp,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	size_t original_buflen, data_buflen, data_idlen;
	uint64_t *data_ids;
	char *buf;
	int error;
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
		error = sls_read_slos_datarec(
		    slsp, data_ids[i], rectable, objtable);
		if (error != 0)
			return (error);
	}

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

	sckpt = slsckpt_alloc(&slsp->slsp_attr);
	if (sckpt == NULL)
		return (ENOMEM);

	/* Read the manifest, get the record number for the checkpoint. */
	error = sls_read_slos_manifest(slsp->slsp_oid, &buf, &buflen);
	if (error != 0) {
		slsckpt_drop(sckpt);
		return (error);
	}

	input_buf = buf;

	/* Extract the list of VM records, return the array of metadata. */
	error = sls_read_slos_datarec_all(
	    slsp, &buf, &buflen, sckpt->sckpt_rectable, objtable);
	if (error != 0)
		goto error;

	/* Extract all metadata records. */
	error = sls_readmeta_slos(buf, buflen, sckpt->sckpt_rectable);
	if (error != 0)
		goto error;

	free(input_buf, M_SLSMM);

	taskqueue_drain_all(slos.slos_tq);

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

static int
sls_createmeta_slos(uint64_t oid, struct vnode **vpp)
{
	int error;

	/* Try to create the node, if not already there, wrap it in a vnode. */
	error = slos_svpalloc(&slos, MAKEIMODE(VREG, S_IRWXU), &oid);
	if (error != 0)
		return (error);

	/* XXX Destroy the vnode if things go sideways. */
	error = VFS_VGET(slos.slsfs_mount, oid, LK_EXCLUSIVE, vpp);
	if (error != 0)
		return (error);

	return (0);
}

/*
 * Creates a record in the SLOS with the metadata held in the sbuf.
 * The record is contiguous, and only has data in the beginning.
 */
static int
sls_writemeta_slos(
    struct sls_record *rec, struct vnode **vpp, bool overwrite, uint64_t offset)
{
	struct sbuf *sb = rec->srec_sb;
	struct thread *td = curthread;
	int mode = FREAD | FWRITE;
	int error, close_error;
	struct slos_rstat st;
	struct vnode *vp;
	void *record;
	size_t len;

	if (vpp != NULL)
		*vpp = NULL;

	record = sbuf_data(sb);
	if (record == NULL)
		return (EINVAL);

	len = sbuf_len(sb);

	/* Try to create the node, if not already there, wrap it in a vnode. */
	error = sls_createmeta_slos(rec->srec_id, &vp);
	if (error != 0)
		return (error);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0)
		goto error;

	st.type = rec->srec_type;
	st.len = offset + len;

	KASSERT(st.type != 0, ("invalid record type"));
	KASSERT(st.len > 0, ("Writing out empty record of type %ld", st.type));

	VOP_UNLOCK(vp, 0);
	error = VOP_IOCTL(vp, SLS_SET_RSTAT, &st, 0, NULL, td);
	VOP_LOCK(vp, LK_EXCLUSIVE);
	if (error != 0) {
		DEBUG1("setting record type failed with %d\n", error);
		return (error);
	}

	/* Keep reading until we get all the info. */
	error = sls_doio(vp, record, len, offset, UIO_WRITE);
	if (error != 0)
		goto error;

	/* Pass the open vnode to the caller if needed. */
	if (vpp != NULL) {
		*vpp = vp;
		return (0);
	}

	/* Else we're done with the vnode. */
	error = VOP_CLOSE(vp, mode, NULL, td);
	if (error != 0)
		return (error);

	vput(vp);

	return (0);

error:
	if (vp != NULL) {
		close_error = VOP_CLOSE(vp, mode, NULL, td);
		if (close_error != 0)
			SLS_DBG("error %d, could not close SLSFS vnode\n",
			    close_error);

		vput(vp);
	}

	if (vpp != NULL)
		*vpp = NULL;

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
	int mode = FREAD | FWRITE;
	struct slsvmobject *vminfo;
	struct vnode *vp, **invp;
	vm_object_t obj;
	size_t offset;
	int error, ret = 0;
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
		invp = (i == amplification - 1) ? &vp : NULL;
		error = sls_writemeta_slos(rec, invp, false, offset);
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
		ret = sls_writeobj_data(vp, obj, offset);
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

	error = VOP_CLOSE(vp, mode, NULL, td);
	if (error != 0) {
		SLS_DBG("error %d could not close slos node\n", error);
		return (ret);
	}

	vput(vp);

	return (ret);
}

static int
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
			printf("Writing to the SLOS failed with %d\n", error);
			return (error);
		}
	}

	return (0);
}

/*
 * XXX Check if the taskqueue actually helps us saturate the disk/network,
 * it is possible that in a lot of cases issuing the IOs is very cheap
 * compared to actually servicing them in the lower layers.
 */
static void
slstable_task(void *ctx, int __unused pending)
{
	struct slstable_taskctx *task = (struct slstable_taskctx *)ctx;
	struct sls_record *rec = task->rec;
	int error;

	/*
	 * VM object records are special, since we need to dump
	 * actual memory along with the metadata.
	 */
	error = sls_writemeta_slos(rec, NULL, true, 0);
	if (error != 0)
		printf("Writing to the SLOS failed with %d\n", error);

	uma_zfree(slstable_task_zone, task);
}

/*
 * Dumps a table of records into the SLOS. The records of VM objects
 * are special cases, since they are sparse and hold all pages that
 * need to be sent to the SLOS.
 */
int
sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt)
{
	struct sbuf *sb_manifest, *sb_meta, *sb_data;
	uint64_t data_records = 0;
	struct sls_record *rec;
	struct slskv_iter iter;
	uint64_t slsid;
	size_t len;
	int error;

	/* Create a record for metadata, data, and the manifest itself. */
	sb_manifest = sbuf_new_auto();
	sb_meta = sbuf_new_auto();
	sb_data = sbuf_new_auto();

	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{
		/* Create eachkrecord in parallel. */
		if (sls_isdata(rec->srec_type)) {
			error = sls_writedata_slos(rec, sckpt);
			if (error != 0)
				printf("Writing to the SLOS failed with %d\n",
				    error);

			/*
			 * Attach the new record directly to the manifest.
			 * The end result is an array of data record OIDs, then
			 * an array of (record, len, string) metadata records.
			 */
			error = sbuf_bcat(
			    sb_data, &rec->srec_id, sizeof(rec->srec_id));
			if (error != 0) {
				KV_ABORT(iter);
				goto out;
			}
			data_records += 1;
		} else {
			/*
			 * Append the metadata record, length, and data.
			 */
			error = sbuf_bcat(sb_meta, rec, sizeof(*rec));
			if (error != 0)
				goto out;

			len = sbuf_len(rec->srec_sb);
			error = sbuf_bcat(sb_meta, &len, sizeof(size_t));
			if (error != 0)
				goto out;

			error = sbuf_bcat(sb_meta, sbuf_data(rec->srec_sb),
			    sbuf_len(rec->srec_sb));
			if (error != 0)
				goto out;
		}
	}

	/* We got all the metadata and data. */
	sbuf_finish(sb_data);
	sbuf_finish(sb_meta);

	/*
	 * XXX INTERMEDIATE PIVOT SOLUTION: First append all VM object IDs to
	 * the manifest. Write out an SLS ID of 0. Then write out the
	 * concatenation of all metadata from the manifest.
	 *
	 * When we read it back in slos_read_manifest we can use strnlen to find
	 * the list of VM object SLSIDs. We then parse out the rest of the
	 * records.
	 *
	 * THE CORRECT SOLUTION: Don't add the records in the table in the first
	 * place, use the record table to prevent duplication only. Directly
	 * append records and data to a unified metadata record.
	 */

	/*
	 * Prepend the data array with its size.
	 */
	error = sbuf_bcat(sb_manifest, &data_records, sizeof(data_records));
	if (error != 0)
		goto out;

	error = sbuf_bcat(sb_manifest, sbuf_data(sb_data), sbuf_len(sb_data));
	if (error != 0)
		goto out;

	error = sbuf_bcat(sb_manifest, sbuf_data(sb_meta), sbuf_len(sb_meta));
	if (error != 0)
		goto out;

	/*
	 * Write the huge metadata block.
	 */
	error = sls_write_slos_manifest(oid, sb_manifest);
	if (error != 0)
		goto out;

out:
	sbuf_delete(sb_data);
	sbuf_delete(sb_meta);
	sbuf_delete(sb_manifest);
	taskqueue_drain_all(slsm.slsm_tabletq);

	return (error);
}

int
sls_export_ssparts(void)
{
	size_t ssparts_len = sizeof(ssparts[0]) * SLS_OIDRANGE;
	struct thread *td = curthread;
	int mode = FREAD | FWRITE;
	struct vnode *vp;
	int close_error;
	int error;

	if (!ssparts_imported)
		return (0);

	/* Get the vnode for the record and open it. */
	error = VFS_VGET(
	    slos.slsfs_mount, SLOS_SLSPART_INODE, LK_EXCLUSIVE, &vp);
	if (error != 0)
		return (0);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0) {
		vput(vp);
		return (error);
	}

	error = sls_doio(vp, ssparts, ssparts_len, 0, UIO_WRITE);

	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);
	return (error);
}

int
sls_import_ssparts(void)
{
	size_t ssparts_len = sizeof(ssparts[0]) * SLS_OIDRANGE;
	struct thread *td = curthread;
	int mode = FREAD | FWRITE;
	struct vnode *vp;
	struct vattr va;
	int close_error;
	int error;

	bzero(ssparts, ssparts_len);

	/* Get the vnode for the record and open it. */
	error = sls_createmeta_slos(SLOS_SLSPART_INODE, &vp);
	if (error != 0)
		return (error);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0) {
		vput(vp);
		return (error);
	}

	error = VOP_GETATTR(vp, &va, NULL);
	if (error != 0)
		goto done;

	/* If not created yet, write out the zeroed out array. */
	if (va.va_size == 0)
		error = sls_doio(vp, ssparts, ssparts_len, 0, UIO_WRITE);
	else
		error = sls_doio(vp, ssparts, ssparts_len, 0, UIO_READ);

done:
	if (error == 0)
		ssparts_imported = true;

	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);
	return (error);
}

int
slspre_export(void)
{
	struct thread *td = curthread;
	struct sls_prefault *slspre;
	int mode = FREAD | FWRITE;
	struct vnode *vp;
	int close_error;
	struct sbuf *sb;
	uint64_t objid;
	size_t iosize;
	uint64_t objsize;
	int error;

	if (slos.slsfs_mount == NULL)
		return (0);

	/* Get the vnode for the record and open it. */
	error = VFS_VGET(
	    slos.slsfs_mount, SLOS_SLSPREFAULT_INODE, LK_EXCLUSIVE, &vp);
	if (error != 0)
		return (0);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0) {
		vput(vp);
		return (error);
	}

	sb = sbuf_new_auto();

	KV_FOREACH_POP(slsm.slsm_prefault, objid, slspre)
	{
		error = sbuf_bcat(sb, &objid, sizeof(objid));
		if (error != 0)
			goto done;

		/* Find a way to get bitmap sizes. */
		objsize = slspre->pre_size;
		error = sbuf_bcat(sb, &objsize, sizeof(objsize));
		if (error != 0)
			goto done;

		error = sbuf_bcat(sb, slspre->pre_map, bitstr_size(objsize));
		if (error != 0)
			goto done;

		slspre_destroy(slspre);
	}

	error = sbuf_finish(sb);
	if (error != 0)
		goto done;

	iosize = sbuf_len(sb);

	error = sls_doio(vp, &iosize, sizeof(iosize), 0, UIO_WRITE);
	if (error != 0)
		goto done;

	error = sls_doio(vp, sbuf_data(sb), iosize, sizeof(iosize), UIO_WRITE);
	if (error != 0)
		goto done;

done:
	sbuf_delete(sb);

	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);
	return (error);
}

int
slspre_import(void)
{
	struct thread *td = curthread;
	struct sls_prefault *slspre;
	int mode = FREAD | FWRITE;
	struct vnode *vp;
	struct vattr va;
	uint64_t offset;
	int close_error;
	uint64_t objid;
	size_t bitlen;
	size_t size;
	int error;

	/* XXX Create custom kern_openat() call to remove direct vnode IO. */

	/* Get the vnode for the record and open it. */
	error = sls_createmeta_slos(SLOS_SLSPREFAULT_INODE, &vp);
	if (error != 0)
		return (error);

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0) {
		vput(vp);
		return (error);
	}

	error = VOP_GETATTR(vp, &va, NULL);
	if (error != 0)
		goto done;

	if (va.va_size == 0)
		goto done;

	error = sls_doio(vp, &size, sizeof(size), 0, UIO_READ);

	offset = sizeof(size);
	if (error != 0)
		goto done;

	while (size > 0) {
		error = sls_doio(vp, &objid, sizeof(objid), offset, UIO_READ);
		if (error != 0)
			goto done;

		size -= sizeof(objid);
		offset += sizeof(objid);

		error = sls_doio(vp, &bitlen, sizeof(bitlen), offset, UIO_READ);
		if (error != 0)
			goto done;

		size -= sizeof(bitlen);
		offset += sizeof(bitlen);

		error = slspre_create(bitlen, &slspre);
		if (error != 0)
			goto done;

		error = sls_doio(
		    vp, slspre->pre_map, bitstr_size(bitlen), offset, UIO_READ);
		if (error != 0)
			goto done;

		size -= bitstr_size(bitlen);
		offset += bitstr_size(bitlen);

		error = slskv_add(slsm.slsm_prefault, objid, (uintptr_t)slspre);
		if (error != 0)
			goto done;
	}

done:
	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);
	return (error);
}
