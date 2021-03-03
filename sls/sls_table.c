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

	taskqueue_drain_all(slos.slos_tq);

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
	} else {
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
