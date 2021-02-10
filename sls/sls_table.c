#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
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
#include "sls_table.h"

#ifdef SLS_TEST
#include "slstable_test.h"
#endif /* SLS_TEST */

/* The maximum size of a single data transfer */
uint64_t sls_contig_limit = MAXBCACHEBUF;
int sls_drop_io = 0;
size_t sls_metadata_sent = 0;
size_t sls_metadata_received = 0;
size_t sls_data_sent = 0;
size_t sls_data_received = 0;
uint64_t sls_pages_grabbed = 0;
uint64_t sls_io_initiated = 0;
unsigned int sls_async_slos = 1;

/*
 * A list of info structs which can hold data,
 * along with the size of their metadata.
 */
static uint64_t sls_datastructs[] = {
	SLOSREC_VMOBJ,
#ifdef SLS_TEST
	SLOSREC_TESTDATA,
#endif
};

static int
sls_doio(struct vnode *vp, struct uio *auio)
{
	int error = 0;
	size_t iosize = 0;
	uint64_t back = 0;

	ASSERT_VOP_LOCKED(vp, ("vnode %p is unlocked", vp));

	/* If we don't want to do anything just return. */
	if (sls_drop_io) {
		auio->uio_resid = 0;
		return (0);
	}

	/* Do the IO itself. */
	iosize = auio->uio_resid;

	while (auio->uio_resid > 0) {
		back = auio->uio_resid;
		if (auio->uio_rw == UIO_WRITE) {
			error = VOP_WRITE(vp, auio, 0, NULL);
		} else {
			error = VOP_READ(vp, auio, 0, NULL);
		}
		if (error != 0) {
			goto out;
		}
		MPASS(back != auio->uio_resid);
	}
	if (auio->uio_rw == UIO_WRITE)
		sls_metadata_sent += iosize;
	else
		sls_metadata_received += iosize;
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
	rec = malloc(sizeof(*rec), M_SLSREC, M_WAITOK);
	rec->srec_id = slsid;
	rec->srec_sb = sb;
	rec->srec_type = type;

	return (rec);
}

void
sls_record_destroy(struct sls_record *rec)
{
	sbuf_delete(rec->srec_sb);
	free(rec, M_SLSREC);
}

static int
sls_isdata(uint64_t type)
{
	int i;

	for (i = 0; i < sizeof(sls_datastructs) / sizeof(*sls_datastructs);
	     i++) {
		if (sls_datastructs[i] == type)
			return (1);
	}

	return (0);
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

static int
sls_readrec_raw(struct vnode *vp, size_t len, char *buf)
{
	struct iovec aiov;
	struct uio auio;
	int error;

	/* Create the UIO for the disk. */
	slos_uioinit(&auio, 0, UIO_READ, &aiov, 1);

	auio.uio_resid = len;
	aiov.iov_base = buf;
	aiov.iov_len = len;
	error = sls_doio(vp, &auio);
	if (error != 0)
		return (error);

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
	error = sls_readrec_raw(vp, len, buf);
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
 * Bring in a record from the backend.
 */
static int
sls_readrec_slos(struct vnode *vp, uint64_t slsid, struct sls_record **recp)
{
	struct sls_record *rec;
	struct slos_rstat st;
	struct sbuf *sb;
	int error;

	/* Get the record type. */
	error = sls_get_rstat(vp, &st);
	if (error != 0) {
		SLS_DBG("getting record type failed with %d\n", error);
		return (error);
	}

	KASSERT(st.len != 0,
	    ("record of type %lx with SLS ID %lx is empty\n", st.type, slsid));

	error = sls_readrec_buf(vp, st.len, &sb);
	if (error != 0)
		return (error);

	/* Bundle the data into an SLS record. */
	rec = sls_getrecord(sb, slsid, st.type);
	*recp = rec;

	return (0);
}

/*
 * Reads in a metadata record representing one or more SLS info structs.
 */
static int
sls_readmeta_slos(struct vnode *vp, uint64_t slsid, struct slskv_table *table)
{
	struct sls_record *rec;
	int error;

	error = sls_readrec_slos(vp, slsid, &rec);
	if (error != 0)
		return (error);

	/* Add the record to the table to be parsed into info structs later. */
	error = slskv_add(table, slsid, (uintptr_t)rec);
	if (error != 0) {
		sls_record_destroy(rec);
		return (error);
	}

	return (0);
}

/* Check if a VM object record is not anonymous, and if so, store it.  */
static int
sls_readdata_slos_vmobj(struct slskv_table *table, uint64_t slsid,
    struct sls_record *rec, vm_object_t *objp)
{
	struct slsvmobject *info;
#ifdef SLS_TEST
	struct data_info *dinfo;
#endif /* SLS_TEST */
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
#ifdef SLS_TEST
	switch (rec->srec_type) {
	case SLOSREC_VMOBJ:
		info = (struct slsvmobject *)sbuf_data(rec->srec_sb);
		type = info->type;
		size = info->size;
		break;
	case SLOSREC_TESTDATA:
		dinfo = (struct data_info *)sbuf_data(rec->srec_sb);
		type = OBJT_DEFAULT;
		size = dinfo->size;
		break;
	default:
		panic("invalid record type %lx", rec->srec_type);
	}

#else

	KASSERT(rec->srec_type == SLOSREC_VMOBJ,
	    ("invalid record type %lx", rec->srec_type));
	info = (struct slsvmobject *)sbuf_data(rec->srec_sb);
	type = info->type;
	size = info->size;

#endif /* SLS_TEST */

	if ((type != OBJT_DEFAULT) && (type != OBJT_SWAP)) {
		/* There no object to read. */
		*objp = NULL;
		return (0);
	}

	/* Allocate a new object. */
	*objp = vm_pager_allocate(OBJT_SWAP, (void *)slsid, IDX_TO_OFF(size),
	    VM_PROT_DEFAULT, 0, NULL);

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

		bp = sls_pager_readbuf(obj, pindex, npages);
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
	sls_data_received += bp->b_resid;
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

/*
 * Reads in a sparse record representing a VM object,
 * and stores it as a linked list of page runs.
 */
static int
sls_readdata(struct vnode *vp, uint64_t slsid, uint64_t type,
    struct slskv_table *rectable, struct slskv_table *objtable, int lazyrest)
{
	struct sls_record *rec;
	vm_object_t obj = NULL;
	struct sbuf *sb;
	size_t len;
	int error;

	switch (type) {
	case SLOSREC_VMOBJ:
		len = sizeof(struct slsvmobject);
		break;

#ifdef SLS_TEST
	case SLOSREC_TESTDATA:
		len = sizeof(struct data_info);
		break;
#endif

	default:

		return (EINVAL);
	}

	/*
	 * Read the object from the SLOS. Seeks for SLOS nodes are block-sized,
	 * so just read the whole first block of the node.
	 */
	error = sls_readrec_buf(vp, len, &sb);
	if (error != 0)
		return (error);

	rec = sls_getrecord(sb, slsid, type);

	/* Store the record for later and possibly make a new object.  */
	VOP_UNLOCK(vp, 0);
	error = sls_readdata_slos_vmobj(rectable, slsid, rec, &obj);
	VOP_LOCK(vp, LK_EXCLUSIVE);
	if (error != 0)
		return (error);

	/* If there is no object to restore right now, we're done. */
	if (obj == NULL)
		return (0);

	/* XXX Add VFS backend handling. */
	if (lazyrest == 0) {
		VOP_UNLOCK(vp, 0);
		error = sls_readdata_slos(vp, obj);
		VOP_LOCK(vp, LK_EXCLUSIVE);

		if (error != 0)
			goto error;
	}

	/* Add the object to the table. */
	error = slskv_add(objtable, slsid, (uintptr_t)obj);
	if (error != 0)
		goto error;

	return (0);

error:
	VM_OBJECT_WUNLOCK(obj);
	vm_object_deallocate(obj);
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

static int
sls_read_slos_manifest(uint64_t oid, uint64_t **ids, size_t *idlen)
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
		SLS_DBG("getting record type failed with %d\n", error);
		vput(vp);
		return (error);
	}

	buf = malloc(st.len, M_SLSMM, M_WAITOK);
	error = sls_readrec_raw(vp, st.len, buf);
	if (error != 0) {
		free(buf, M_SLSMM);
		vput(vp);
		return (error);
	}

	*ids = (uint64_t *)buf;
	*idlen = st.len / sizeof(**ids);

	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);

	return (0);
}

static int
sls_read_slos_record(uint64_t oid, struct slskv_table *rectable,
    struct slskv_table *objtable, int lazyrest)
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
	VOP_UNLOCK(vp, LK_EXCLUSIVE);
	error = VOP_IOCTL(vp, SLS_GET_RSTAT, &st, 0, NULL, td);
	vn_lock(vp, LK_EXCLUSIVE);
	if (error != 0) {
		vput(vp);
		DEBUG1("getting record type failed with %d\n", error);
		return (error);
	}

	/*
	 * If the record can hold data, "typecast" it to look like it only has
	 * metadata. We will manually read the data later.
	 */
	DEBUG1("Read record of type %d", st.type);
	if (sls_isdata(st.type)) {
		ret = sls_readdata(
		    vp, oid, st.type, rectable, objtable, lazyrest);
		if (ret != 0)
			goto out;
	} else {
		ret = sls_readmeta_slos(vp, oid, rectable);
		if (ret != 0)
			goto out;
	}

out:
	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		DEBUG1("error %d, could not close slos node", close_error);

	vput(vp);

	return (ret);
}

/* Reads in a record from the SLOS and saves it in the record table. */
int
sls_read_slos(struct slspart *slsp, struct slskv_table **rectablep,
    struct slskv_table *objtable)
{
	struct slskv_table *rectable;
	uint64_t *ids = NULL;
	size_t idlen;
	int error, i;

	/* Create the tables for the data and metadata of the checkpoint. */
	error = slskv_create(&rectable);
	if (error != 0)
		return (error);

	/* Read the manifest, get the record number for the checkpoint. */
	error = sls_read_slos_manifest(slsp->slsp_oid, &ids, &idlen);
	if (error != 0)
		goto error;

	for (i = 0; i < idlen; i++) {
		error = sls_read_slos_record(
		    ids[i], rectable, objtable, SLSPART_LAZYREST(slsp));
		if (error != 0)
			goto error;
	}

	*rectablep = rectable;

	free(ids, M_SLSMM);

	return (0);

error:
	free(ids, M_SLSMM);
	sls_free_rectable(rectable);

	return (error);
}

/*
 * Get the pages of an object in the form of a
 * linked list of contiguous memory areas.
 *
 * This function is not inlined in order to be able to use DTrace on it.
 */
static int __attribute__((noinline))
sls_writeobj_data(struct vnode *vp, vm_object_t obj)
{
	vm_pindex_t pindex;
	struct buf *bp;
	int error;

	pindex = 0;
	for (;;) {
		/*
		 * Split the object into contiguous chunks, send it to the disk.
		 * Every logically contiguous chunk is physically contiguous in
		 * backing storage.
		 */
		bp = sls_pager_writebuf(obj, pindex, sls_contig_limit);
		if (bp == NULL)
			break;
		/*
		 * The pindex from which we're going to search for the next run
		 * of pages.
		 */
		pindex = bp->b_pages[bp->b_npages - 1]->pindex + 1;

		/* Update the counter. */
		sls_data_sent += bp->b_resid;

		BUF_ASSERT_LOCKED(bp);
		error = slos_iotask_create(vp, bp, sls_async_slos);
		if (error != 0) {
			return (error);
		}
	}

	return (0);
}

/*
 * Creates a record in the SLOS with the metadata held in the sbuf.
 * The record is contiguous, and only has data in the beginning.
 */
static int
sls_writemeta_slos(struct sls_record *rec, struct vnode **vpp, bool overwrite)
{
	struct sbuf *sb = rec->srec_sb;
	struct thread *td = curthread;
	int mode = FREAD | FWRITE;
	int error, close_error;
	struct slos_rstat st;
	struct vnode *vp;
	uint64_t oid;
	struct iovec aiov;
	struct uio auio;
	void *record;
	size_t len;

	if (vpp != NULL)
		*vpp = NULL;

	record = sbuf_data(sb);
	if (record == NULL)
		return (EINVAL);

	len = sbuf_len(sb);

	/* Try to create the node, if not already there, wrap it in a vnode. */
	oid = rec->srec_id;
	error = slos_svpalloc(&slos, MAKEIMODE(VREG, S_IRWXU), &oid);
	if (error != 0)
		return (error);

	error = VFS_VGET(slos.slsfs_mount, oid, LK_EXCLUSIVE, &vp);
	if (error != 0)
		return (error);

	/*
	 * If the node is just for metadata, delete the previous contents.
	 * This is because some records include null-terminated arrays.
	 */
	if (overwrite) {
		/* XXX Truncate */
		/*
		   error = slsfs_truncate(vp, 0);
		   if (error != 0)
		   goto error;
		   */
	}

	/* Open the record for writing. */
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0)
		goto error;

	st.type = rec->srec_type;
	st.len = len;

	KASSERT(st.type != 0, ("invalid record type"));
	KASSERT(st.len > 0, ("Writing out empty record of type %ld", st.type));

	VOP_UNLOCK(vp, 0);
	error = VOP_IOCTL(vp, SLS_SET_RSTAT, &st, 0, NULL, td);
	VOP_LOCK(vp, LK_EXCLUSIVE);
	if (error != 0) {
		DEBUG1("setting record type failed with %d\n", error);
		return (error);
	}

	/* Create the UIO for the disk. */
	aiov.iov_base = record;
	aiov.iov_len = len;
	slos_uioinit(&auio, 0, UIO_WRITE, &aiov, 1);

	/* Keep reading until we get all the info. */
	error = sls_doio(vp, &auio);
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
sls_writedata_slos(struct sls_record *rec, struct slskv_table *objtable)
{
	struct sbuf *sb = rec->srec_sb;
	struct thread *td = curthread;
	int mode = FREAD | FWRITE;
	struct slsvmobject *vminfo;
	vm_object_t obj;
	struct vnode *vp;
	int error, ret = 0;
#ifdef SLS_TEST
	struct data_info *datainfo;
#endif /* SLS_TEST */

	/* The record number returned is the unique record ID. */
	error = sls_writemeta_slos(rec, &vp, false);
	if (error != 0)
		return (error);

	/* The node will exist, since we called sls_writemeta_slos().*/
	error = VOP_OPEN(vp, mode, NULL, td, NULL);
	if (error != 0)
		return (error);

	/*
	 * We know that the sbuf holds a slsvmobject struct,
	 * that's why we are in this function in the first place.
	 */
	if (rec->srec_type == SLOSREC_VMOBJ) {
		vminfo = (struct slsvmobject *)sbuf_data(sb);
		/* Get the object itself. */
		obj = (vm_object_t)vminfo->objptr;
	}
#ifdef SLS_TEST
	else if (rec->srec_type == SLOSREC_TESTDATA) {
		datainfo = (struct data_info *)sbuf_data(sb);
		/* The slsid is always a pointer for data objects. */
		obj = (vm_object_t)datainfo->slsid;
	}
#endif /* SLS_TEST */
	else {
		panic("invalid type %lx for metadata", rec->srec_type);
		return (error);
	}

	/*
	 * The ID of the info struct and the in-memory pointer
	 * are identical at checkpoint time, so we use it to
	 * retrieve the object and grab its data.
	 */
	if (((obj->type != OBJT_DEFAULT) && (obj->type != OBJT_SWAP)))
		goto out;

	/* Send out the object's data. */
	ret = sls_writeobj_data(vp, obj);
	if (ret != 0)
		goto out;

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

	error = sls_writemeta_slos(&rec, NULL, true);
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
		error = sls_writedata_slos(rec, sckpt_data->sckpt_objtable);
		if (error != 0) {
			KV_ABORT(iter);
			printf("Writing to the SLOS failed with %d\n", error);
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
sls_write_slos(uint64_t oid, struct slsckpt_data *sckpt_data)
{
	struct sbuf *sb_manifest;
	struct sls_record *rec;
	struct slskv_iter iter;
	uint64_t slsid;
	int error;

	sb_manifest = sbuf_new_auto();

	KV_FOREACH(sckpt_data->sckpt_rectable, iter, slsid, rec)
	{
		/*
		 * VM object records are special, since we need to dump
		 * actual memory along with the metadata.
		 */
		if (sls_isdata(rec->srec_type))
			error = sls_writedata_slos(
			    rec, sckpt_data->sckpt_objtable);
		else
			error = sls_writemeta_slos(rec, NULL, true);
		if (error != 0) {
			KV_ABORT(iter);
			printf("Writing to the SLOS failed with %d\n", error);
			goto error;
		}

		/* Attach the new record to the checkpoint manifest. */
		error = sbuf_bcat(
		    sb_manifest, &rec->srec_id, sizeof(rec->srec_id));
		if (error != 0) {
			KV_ABORT(iter);
			goto error;
		}
	}

	error = sls_write_slos_manifest(oid, sb_manifest);
	if (error != 0) {
		printf(
		    "Writing the manifest to the SLOS failed with %d\n", error);
		goto error;
	}

	sbuf_delete(sb_manifest);

	return (0);

error:

	sbuf_delete(sb_manifest);
	return (error);
}

/* ---------------------- TESTING ---------------------- */

#ifdef SLS_TEST

/*
 * Make an artificial metadata record to be used in the test.
 */
static int
slstable_mkinfo_meta(uint64_t slsid, meta_info **minfop, uint64_t type)
{
	meta_info *minfo;
	int i;

	/*
	 * This buffer doesn't get inserted to any table, however it is
	 * where we construct the meta_info object before copying over to the
	 * sbuf.
	 */

	/* Set up the metadata struct, including random data. */
	minfo = malloc(sizeof(*minfo), M_SLSMM, M_WAITOK);
	minfo->magic = (type == SLOSREC_TESTMETA) ? META_MAGIC : DATA_MAGIC;
	minfo->slsid = slsid;

	for (i = 0; i < INFO_SIZE; i++)
		minfo->data[i] = 'a' + random() % ('z' - 'a');

	*minfop = minfo;

	return (0);
}

/*
 * Create a new sbuf and populate it with the contents of a buffer.
 */
static int
slstable_mkinfo_sbuf(struct sbuf **sbp, meta_info *minfo)
{
	struct sbuf *sb;
	int error;

	/* Create a new buffer and associate it with the meta info struct. */
	sb = sbuf_new_auto();

	/* Fill a new buffer with the provided data. */
	error = sbuf_bcpy(sb, minfo, sizeof(*minfo));
	if (error != 0) {
		sbuf_delete(sb);
		return (error);
	}

	/* Finalize the buffer and pass it to the caller. */
	error = sbuf_finish(sb);
	if (error != 0) {
		sbuf_delete(sb);
		return (0);
	}

	*sbp = sb;

	return (0);
}

/*
 * Add a buffer to a table as a record with the given id.
 */
static int
slstable_mkinfo_addrec(
    struct slskv_table *table, uint64_t slsid, meta_info *minfo, uint64_t type)
{
	struct sls_record *rec;
	struct sbuf *sb;
	int error;

	/* Create the sbuf for the table. */
	error = slstable_mkinfo_sbuf(&sb, minfo);
	if (error != 0)
		return (error);

	/* Add the sbuf to the table. */
	rec = sls_getrecord(sb, slsid, type);
	error = slskv_add(table, slsid, (uint64_t)rec);
	if (error != 0) {
		free(rec, M_SLSREC);
		sbuf_delete(sb);
		return (error);
	}

	return (0);
}

/*
 * Create an info struct and insert it to the tables used for bookkeeping.
 * Note that this function only creates meta_info structs, but the two
 * are structurally equivalent.
 */
static int
slstable_mkinfo(struct slsckpt_data *sckpt_data,
    struct slsckpt_data *sckpt_tmpdata, vm_object_t obj)
{
	meta_info *minfo;
	uint64_t slsid;
	uint64_t type;
	int error;

	type = (obj != NULL) ? SLOSREC_TESTDATA : SLOSREC_TESTMETA;
	slsid = (obj != NULL) ? (uint64_t)obj : random();

	/* Create the metadata info struct to be added. */
	error = slstable_mkinfo_meta(slsid, &minfo, type);
	if (error != 0)
		return (error);

	/* Add it to both tables. */
	error = slstable_mkinfo_addrec(
	    sckpt_data->sckpt_rectable, slsid, minfo, type);
	if (error != 0)
		goto out;

	error = slstable_mkinfo_addrec(
	    sckpt_tmpdata->sckpt_rectable, slsid, minfo, type);
	if (error != 0)
		goto out;
out:

	free(minfo, M_SLSMM);

	return (error);
}

/*
 * Test the retrieved metadata by checking it against the original data stored.
 * Also possibly give back the VM object holding the entry's data.
 */
static int
slstable_testmeta(struct slskv_table *origtable, struct sls_record *rec)
{
	struct sls_record *origrec = NULL;
	meta_info *origminfo, *minfo;
	size_t origmlen, mlen;
	int error = 0;

	/* Find the sbuf that corresponds to the new element. */
	error = slskv_find(origtable, rec->srec_id, (uintptr_t *)&origrec);
	if (error != 0) {
		printf(
		    "ERROR: Retrieved element not found in the original table\n");
		return (EINVAL);
	}

	/*
	 * Check whether the type is correct. This is an introductory test - if
	 * it fails, we have probably read garbage. Note that we have popped the
	 * data from the table, so if we error out we will possibly get a leak,
	 * but if we have evidence of data corruption it's better to leak memory
	 * than try to free a pointer that very possibly points to garbage.
	 */
	if (origrec->srec_type != rec->srec_type) {
		printf("ERROR: Original type is %ld, type found is %ld\n",
		    origrec->srec_type, rec->srec_type);
		return (EINVAL);
	}

	/* Unpack the original record from the sbuf. */
	minfo = (meta_info *)sbuf_data(rec->srec_sb);
	mlen = sbuf_len(rec->srec_sb);
	if (minfo == NULL) {
		printf("No retrieved metadata buffer\n");
		return (EINVAL);
	}

	/* Unpack the original record from the sbuf. */
	origminfo = (meta_info *)sbuf_data(origrec->srec_sb);
	origmlen = sbuf_len(origrec->srec_sb);
	if (origminfo == NULL) {
		printf("No original metadata buffer\n");
		return (EINVAL);
	}

	if (origmlen != mlen) {
		printf("ERROR: Original size is %ld, retrieved is %ld\n",
		    origmlen, mlen);
		return (EINVAL);
	}

	/* Compare the original and retrieved records. They should be identical.
	 */
	if (memcmp(origminfo->data, minfo->data, INFO_SIZE)) {
		printf("ERROR: Retrieved record differs from the original\n");
		printf("data: %.*s\n", INFO_SIZE, origminfo->data);
		printf("Retrieved data: %.*s\n", INFO_SIZE, minfo->data);
		return (EINVAL);
	}

	return (0);
}

/*
 * Fill a VM object page with data.
 */
static void
slstable_mkdata_populate(vm_object_t obj, vm_pindex_t pindex)
{
	uint8_t *data;
	vm_page_t m;
	int i;

	VM_OBJECT_WLOCK(obj);

	/*
	 * Get a new page for the object and fill it with data. This is the same
	 * procedure as when restoring the pages of a process, except that here
	 * we fill them with random data.
	 */
	m = vm_page_alloc(obj, pindex, VM_ALLOC_NORMAL | VM_ALLOC_WAITOK);
	KASSERT(m != NULL, ("vm_page_alloc failed"));
	KASSERT(m->pindex == pindex,
	    ("asked for pindex %lx, got %lx", pindex, m->pindex));

	/* Map it into the kernel, fill it with data. */
	data = (char *)pmap_map(NULL, m->phys_addr, m->phys_addr + PAGE_SIZE,
	    VM_PROT_READ | VM_PROT_WRITE);

	for (i = 0; i < PAGE_SIZE; i++)
		data[i] = 'a' + (random() % ('z' - 'a'));

	m->valid = VM_PAGE_BITS_ALL;

	VM_OBJECT_WUNLOCK(obj);
}

/*
 * Create a VM object and add data to it.
 */
static int
slstable_mkdata_object(vm_object_t *objp)
{
	vm_object_t obj;
	int i;

	/*
	 * Create the object to be dumped and restored; we don't need to insert
	 * it anywhere.
	 */
	obj = vm_object_allocate(OBJT_DEFAULT, VMOBJ_SIZE);

	/*
	 * For each object we want an average of DATA_SIZE pages
	 * to be resident out of VMOBJ_SIZE possible pages. We
	 * approximate this ratio by having each page be resident
	 * with probability (DATA_SIZE / VMOBJ_SIZE).
	 */
	for (i = 0; i < VMOBJ_SIZE; i++) {
		/* Flip a coin to see if we get this page. */
		if (random() % VMOBJ_SIZE >= DATA_SIZE)
			continue;

		/* Fill the object with data. */
		slstable_mkdata_populate(obj, i);
	}

	printf("Created object %p (%d pages)\n", obj, obj->resident_page_count);
	*objp = obj;

	return (0);
}

static int
slstable_mkdata(struct slsckpt_data *sckpt_data,
    struct slsckpt_data *sckpt_tmpdata, vm_object_t *objp)
{
	vm_object_t obj;
	int error;

	error = slstable_mkdata_object(&obj);
	if (error != 0)
		return (error);

	error = slskv_add(
	    sckpt_data->sckpt_objtable, (uint64_t)obj, (uintptr_t)obj);
	if (error != 0) {
		vm_object_deallocate(obj);
		return (error);
	}

	vm_object_reference(obj);
	error = slskv_add(
	    sckpt_tmpdata->sckpt_objtable, (uint64_t)obj, (uintptr_t)obj);
	if (error != 0) {
		vm_object_deallocate(obj);
		return (error);
	}

	*objp = obj;
	return (0);
}

static int
slstable_testpage_valid(vm_page_t m)
{
	char *buf;
	char c;
	int i;

	buf = (char *)PHYS_TO_DMAP(m->phys_addr);
	for (i = 0; i < PAGE_SIZE; i++) {
		c = buf[i];
		if (c < 'a' || c > 'z') {
			printf("Object illegal value 0x%x "
			       "in page with pindex %ld, offset %d\n",
			    c, m->pindex, i);
			return (EINVAL);
		}
	}

	return (0);
}

/* Make sure the object has the data we added to it. */
static int
slstable_testobj_valid(vm_object_t obj)
{
	vm_page_t m;

	KASSERT(obj->type == OBJT_SWAP || obj->type == OBJT_DEFAULT,
	    ("object %p has illegal type %d", obj, obj->type));
	KASSERT(obj->ref_count > 0, ("object %p has no references", obj));
	TAILQ_FOREACH (m, &obj->memq, listq) {
		if (slstable_testpage_valid(m) != 0)
			return (EINVAL);
	}

	return (0);
}

static int
slstable_testobjtable_valid(struct slskv_table *objtable)
{
	struct slskv_iter iter;
	uint64_t slsid;
	vm_object_t obj;
	int error;

	KV_FOREACH(objtable, iter, slsid, obj)
	{
		error = slstable_testobj_valid(obj);
		if (error != 0)
			return (error);
	}

	return (0);
}

static int
slstable_testdata(
    struct slskv_table *objtable, uint64_t slsid, vm_object_t oldobj)
{
	const int cmpsize = 32;
	vm_page_t m, oldm;
	vm_object_t obj;
	int error = 0;
	char *buf, *oldbuf;

	/* Find the object in the table. */
	error = slskv_find(objtable, slsid, (uintptr_t *)&obj);
	if (error != 0) {
		printf("Restored object not found\n");
		vm_object_deallocate(oldobj);
		return (error);
	}

	slskv_del(objtable, slsid);

	if (slstable_testobj_valid(obj) != 0) {
		printf("Restored object %p is inconsistent\n", obj);
		goto out;
	}

	if (oldobj->resident_page_count != obj->resident_page_count) {
		printf("Original object has %d pages, new one has %d\n",
		    oldobj->resident_page_count, obj->resident_page_count);

		goto out;
	}

	/* Compare all pages one by one. */
	VM_OBJECT_WLOCK(obj);
	TAILQ_FOREACH (oldm, &oldobj->memq, listq) {
		/* Check if we found a page in the same index. */
		m = vm_page_find_least(obj, oldm->pindex);
		if (m->pindex != oldm->pindex) {
			printf("Page with pindex %ld not found "
			       "in new object (minimal index %ld)\n",
			    oldm->pindex, m->pindex);
			VM_OBJECT_WUNLOCK(obj);
			goto out;
		}

		KASSERT(
		    m->psind == oldm->psind, ("pages have different sizes"));

		/* Compare the data itself. */
		buf = (char *)PHYS_TO_DMAP(m->phys_addr);
		oldbuf = (char *)PHYS_TO_DMAP(oldm->phys_addr);
		if (memcmp(buf, oldbuf, PAGE_SIZE) != 0) {
			printf("Pages with pindex %lx have different data\n",
			    oldm->pindex);
			printf("Old: %.*s\n", cmpsize, oldbuf);
			printf("New: %.*s\n", cmpsize, buf);
		}

		/* Remove page from the object, free to the queues if unwired */

		vm_page_lock(m);
		vm_page_assert_locked(m);
		if (vm_page_remove(m))
			vm_page_free(m);
		vm_page_unlock(m);
	}
	VM_OBJECT_WUNLOCK(obj);

	/* Check if the new object has any pages not in the original. */
	if (obj->resident_page_count != 0) {
		printf("New object has %d extra pages at indices:",
		    obj->resident_page_count);
		TAILQ_FOREACH (m, &obj->memq, listq)
			printf("%lx ", m->pindex);
		printf("\n");
	}

out:

	return (error);
}

int
slstable_test(void)
{
	struct slskv_table *objtable = NULL, *rectable = NULL;
	struct slsckpt_data *sckpt_data = NULL, *sckpt_tmpdata = NULL;
	struct slspart *slsp = NULL;
	struct sls_record *rec;
	struct sls_attr attr;
	vm_object_t obj = NULL;
	uint64_t lost_elements;
	struct slskv_iter iter;
	uint64_t old_async;
	struct sbuf *sb;
	uint64_t slsid;
	uint64_t type;
	int error, i;

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = 0,
	};

	/*
	 * Save the old sysctl values, set up our own.
	 */
	old_async = sls_async_slos;

	sls_async_slos = 1;

	error = slsp_add(TEST_PARTID, attr, &slsp);
	if (error != 0)
		goto out;

	/*
	 * We create two checkpoints, one of which will be consumed in the
	 * writing process.
	 */
	error = slsckpt_create(&sckpt_data, &attr);
	if (error != 0) {
		SLS_ERROR(slsckpt_create, error);
		goto out;
	}

	error = slsckpt_create(&sckpt_tmpdata, &attr);
	if (error != 0) {
		SLS_ERROR(slsckpt_create, error);
		goto out;
	}

	/*
	 * Create the pure metadata objects. These are straightforward, because
	 * they are just binary blobs - they don't have actual user data that
	 * needs saving.
	 */
	for (i = 0; i < META_INFOS; i++) {
		error = slstable_mkinfo(sckpt_data, sckpt_tmpdata, NULL);
		if (error != 0) {
			SLS_ERROR(slstable_mkinfo, error);
			goto out;
		}
	}

	for (i = 0; i < DATA_INFOS; i++) {
		/* Create the data-holding objects. */
		error = slstable_mkdata(sckpt_data, sckpt_tmpdata, &obj);
		if (error != 0)
			goto out;

		/* Create the data-holding objects. */
		/* For each object, create an sbuf, associate the two. */
		error = slstable_mkinfo(sckpt_data, sckpt_tmpdata, obj);
		if (error != 0) {
			vm_object_deallocate(obj);
			goto out;
		}
	}

	error = slstable_testobjtable_valid(sckpt_data->sckpt_objtable);
	if (error != 0) {
		printf("Original object table not valid\n");
		goto out;
	}

	/* Write the data generated to the SLOS. */
	error = sls_write_slos(TEST_PARTID, sckpt_tmpdata);
	if (error != 0) {
		SLS_ERROR(sls_write_slos, error);
		goto out;
	}

	error = slstable_testobjtable_valid(sckpt_data->sckpt_objtable);
	if (error != 0) {
		printf("Original object table not valid after write\n");
		goto out;
	}

	/* Wait until every IO has truly hit the disk. */
	taskqueue_drain_all(slos.slos_tq);
	VFS_SYNC(slos.slsfs_mount, MNT_WAIT);

	slsckpt_destroy(sckpt_tmpdata, NULL);
	sckpt_tmpdata = NULL;

	error = slstable_testobjtable_valid(sckpt_data->sckpt_objtable);
	if (error != 0) {
		printf("Original object table not valid after sync\n");
		goto out;
	}

	/* Read the data back. */
	error = sls_read_slos(slsp, &rectable, &objtable);
	if (error != 0) {
		SLS_ERROR(sls_read_slos, error);
		goto out;
	}

	error = slstable_testobjtable_valid(sckpt_data->sckpt_objtable);
	if (error != 0) {
		printf("Original object table not valid after read\n");
		goto out;
	}

	/*
	 * Pop all entries in the metadata table. For each entry we conduct a
	 * number of tests, including:
	 * - Is the type as expected?
	 * - Does the element correspond to an sbuf in the original table?
	 * - Is the struct identical to the one in the original table?
	 * - If the entry is a data entry, also check its data.
	 */
	KV_FOREACH(rectable, iter, slsid, rec)
	{
		switch (rec->srec_type) {
		case SLOSREC_TESTMETA:
		case SLOSREC_TESTDATA:
			/* Test only the metadata, since there is no data. */
			error = slstable_testmeta(
			    sckpt_data->sckpt_rectable, rec);
			if (error != 0) {
				KV_ABORT(iter);
				SLS_ERROR(slstable_testmeta, error);
				goto out;
			}

			break;

		default:
			printf("ERROR: Invalid type found: %ld.\n",
			    rec->srec_type);
			printf("Valid types: %x, %x\n", SLOSREC_TESTMETA,
			    SLOSREC_TESTDATA);
			KV_ABORT(iter);
			goto out;
		}
	}

	/* Check if all objects have been restored correctly. */
	KV_FOREACH_POP(sckpt_data->sckpt_objtable, slsid, obj)
	{
		error = slstable_testdata(objtable, slsid, obj);
		if (error != 0) {
			SLS_ERROR(slstable_testdata, error);
			goto out;
		}
	}

	lost_elements = 0;
	KV_FOREACH_POP(sckpt_data->sckpt_objtable, sb, type)
	{
		sbuf_delete(sb);
		lost_elements += 1;
	}

	if (lost_elements != 0) {
		printf(
		    "ERROR: Lost %lu objects between checkpoint and restore\n",
		    lost_elements);
		goto out;
	}

	printf("Everything went OK.\n");

out:

	if (sckpt_data != NULL) {
		/* Pop and free. */
		slsckpt_destroy(sckpt_data, NULL);
	}

	/* The backup table has the same data as the original - all sbufs have
	 * been destroyed. */
	if (sckpt_tmpdata != NULL)
		slsckpt_destroy(sckpt_tmpdata, NULL);

	if (rectable != NULL)
		sls_free_rectable(rectable);

	if (slsp != NULL)
		slsp_del(TEST_PARTID);

	/* Restore the old sysctl values. */
	sls_async_slos = old_async;

	return (error);
}

#endif /* SLS_TEST */
