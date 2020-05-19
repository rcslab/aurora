#include <sys/types.h>

#include <sys/conf.h>
#include <sys/filio.h>
#include <sys/kernel.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/mman.h>
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
#include <sys/uio.h>

#include <machine/param.h>
#include <machine/reg.h>
#include <machine/vmparam.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <slos.h>
#include <slsfs.h>
#include <sls_data.h>
#include <slos_io.h>
#include <slos_record.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_mm.h"
#include "sls_table.h"

#ifdef SLS_TEST
#include "slstable_test.h"
#endif /* SLS_TEST */


/* The maximum size of a single data transfer */
uint64_t sls_contig_limit = MAXBCACHEBUF;
unsigned int sls_use_nulldev = 0;
int sls_drop_io = 0;
uint64_t sls_iochain_size = 1;
struct file *sls_blackholefp;
size_t sls_bytes_sent;
size_t sls_bytes_received;
uint64_t sls_pages_grabbed;
uint64_t sls_io_initiated;
/* XXX Turn into sysctl */
int sls_async_slos = 0;

uma_zone_t slspagerun_zone = NULL;

/* 
 * A list of info structs which can hold data, 
 * along with the size of their metadata. 
 */
static uint64_t sls_datastructs[] =  { 
	SLOSREC_VMOBJ, 
#ifdef SLS_TEST
	SLOSREC_TESTDATA, 
#endif
};

static int
sls_doio_null(struct uio *auio)
{
	size_t sent;
	int error;

	/* We don't need to do anything for a null IO.
	 * XXX We could use the zero device later for this.
	 */
	if (auio->uio_rw == UIO_READ) {
		auio->uio_resid = 0;
		return (0);
	}

	sent = auio->uio_resid;
	while (auio->uio_resid > 0) {
		error = dofilewrite(curthread, -1, sls_blackholefp, auio, 0, 0);
		if (error != 0)
			return (error);
	}
	sls_bytes_sent += sent;

	return (0);
}

static int
sls_doio(struct vnode *vp, struct uio *auio)
{
	int error = 0;
	size_t iosize = 0;
	char *base;

	/* If we don't want to do anything just return. */
	if (sls_drop_io) {
		auio->uio_resid = 0;
		return (0);
	}

	/* Route the IO to the null device if that's what we want. */
	if (sls_use_nulldev) {
		error = sls_doio_null(auio);
		if (error != 0)
			goto out;
		sls_io_initiated += 1;
		return (0);
	}

#if 0
#define AMPLSIZE 5
	struct iovec aiov[AMPLSIZE];
	for (int i = 0; i < AMPLSIZE; i++)
		aiov[i] = *(auio->uio_iov);

	auio->uio_resid += (AMPLSIZE - 1) * auio->uio_resid;
	auio->uio_iovcnt += (AMPLSIZE - 1);
	auio->uio_iov = aiov;
#endif

	/* Do the IO itself. */
	iosize = auio->uio_resid;
	base = ((char *) auio->uio_iov[0].iov_base);
	while (auio->uio_resid > 0) {
		if (auio->uio_rw == UIO_WRITE)
			error = VOP_WRITE(vp, auio, 0, NULL);
		else
			error = VOP_READ(vp, auio, 0, NULL);
		if (error != 0)
			goto out;
	}
	if (auio->uio_rw == UIO_WRITE)
		sls_bytes_sent += iosize;
	else
		sls_bytes_received += iosize;
out:

	sls_io_initiated += 1;
	if (error != 0)
		printf("ERROR %d\n", error);

	return (error);
}

/* Creates an in-memory Aurora record. */
struct sls_record *
sls_getrecord(struct sbuf *sb, uint64_t slsid, uint64_t type)
{
	struct sls_record *rec;

	rec = malloc(sizeof(*rec), M_SLSMM, M_WAITOK);
	rec->srec_id = slsid;
	rec->srec_sb = sb;
	rec->srec_type = type;

	return (rec);
}

void
sls_record_destroy(struct sls_record *rec)
{
	sbuf_delete(rec->srec_sb);
	free(rec, M_SLSMM);
}

static int
sls_isdata(uint64_t type)
{
	int i;

	for (i = 0; i < sizeof(sls_datastructs) / sizeof(*sls_datastructs); i++) {
		if (sls_datastructs[i] == type)
			return (1);
	}

	return (0);
}

/*
 * Get the size and type of a SLOS vnode.
 */
static int
sls_rstat(struct vnode *vp, struct slos_rstat *st)
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
	 * will be cleaned up with the sbuf.
	 */
	sb = sbuf_new(NULL, buf, len, SBUF_FIXEDLEN);
	if (sb == NULL) {
		free(buf, M_SLSMM);
		return (ENOMEM);
	}
	SBUF_SETFLAG(sb, SBUF_DYNAMIC);

	/* Close up the buffer. */
	error = sbuf_finish(sb);
	if (error != 0) {
		sbuf_delete(sb);
		return (error);
	}

	KASSERT((sb->s_flags & SBUF_FINISHED) == SBUF_FINISHED, ("buffer not finished?"));

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
	buf = SBMALLOC(len);
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
	error = sls_rstat(vp, &st);
	if (error != 0) {
		SLS_DBG("getting record type failed with %d\n", error);
		return (error);
	}

	KASSERT(st.len != 0, ("record with SLS ID %lx is empty\n", slsid));

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
	error = slskv_add(table, slsid, (uintptr_t) rec);
	if (error != 0) {
		sls_record_destroy(rec);
		return (error);
	}

	return (0);
}

/*
 * Reads in a sparse record representing a VM object,
 * and stores it as a linked list of page runs.
 */
static int
sls_readdata_slos(struct vnode *vp, uint64_t slsid, struct slskv_table *rectable,
    struct slskv_table *datatable)
{
	struct thread *td = curthread;
	struct slspagerun *pagerun;
	struct slsdata *data;
	void *pages;
	struct iovec aiov;
	struct uio auio;
	uint64_t offset = 0, pindex;
	uint64_t len;
	int error;


	/* First read the object metadata. The data gets read separately. */
	error = sls_readmeta_slos(vp, slsid, rectable);
	if (error != 0)
		return error;

	/* Start from the first page. */
	/* XXX Change so we get a more dynamic start. */
	offset = 4096;

	/*
	 * The data pages themselves will be added to the data table,
	 * represented as a linked list of page runs. We use the record's
	 * location in memory as the key for the data.
	 */

	data = malloc(sizeof(*data), M_SLSMM, M_WAITOK);
	LIST_INIT(data);

	error = slskv_add(datatable, slsid, (uintptr_t) data);
	if (error != 0)
		return error;

	for (;;) {
		slos_uioinit(&auio, offset, UIO_READ, &aiov, 1);
		/* Find the extent starting with the offset provided. */
		VOP_UNLOCK(vp, 0);
		error = VOP_IOCTL(vp, SLS_SEEK_EXTENT, &auio, 0, NULL, td);
		VOP_LOCK(vp, LK_EXCLUSIVE);
		if (error != 0) {
			SLS_DBG("extent seek failed with %d\n", error);
			return (error);
		}

		/* Get the new extent. If we get no more data, we're done. */
		len = auio.uio_resid;
		if (len == 0)
			break;

		/* Otherwise allocate a buffer for the data. */
		pages = malloc(len, M_SLSMM, M_WAITOK);

		/* Create the UIO for the disk. */
		aiov.iov_base = pages;
		aiov.iov_len = len;

		/* Update the point of the new offset . */
		pindex = OFF_TO_IDX(auio.uio_offset) - 1;
		offset = auio.uio_offset + auio.uio_resid;

		/* The read itself. */
		error =  sls_doio(vp, &auio);
		if (error != 0) {
			free(pages, M_SLSMM);
			return (error);
		}

		/* Add the new pagerun to the list. */
		pagerun = uma_zalloc(slspagerun_zone, M_WAITOK);

		/*
		 * The pages start from the 2nd 4K block in the record.
		 * Moreover, the index of the pages in the object
		 * correspond to page-sized chunks.
		 */
		pagerun->idx = pindex;
		pagerun->len = len;
		pagerun->data = pages;

		/* Increment the offset by the amount of bytes read. */

		LIST_INSERT_HEAD(data, pagerun, next);
	}

	return (0);
}

static void
sls_free_rectable(struct slskv_table *rectable)
{
	struct sls_record *rec;
	uint64_t slsid;

	KV_FOREACH_POP(rectable, slsid, rec)
		sls_record_destroy(rec);

	slskv_destroy(rectable);
}

static void
sls_free_datatable(struct slskv_table *datatable)
{
	struct slspagerun *pagerun, *tmp;
	struct slsdata data;
	uint64_t slsid;

	KV_FOREACH_POP(datatable, slsid, data) {
		LIST_FOREACH_SAFE(pagerun, &data, next, tmp) {
			SLS_BREAKPOINT();
			uma_zfree(slspagerun_zone, pagerun);
			SLS_BREAKPOINT();
		}
	}

	slskv_destroy(datatable);
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
	error = sls_rstat(vp, &st);
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

	*ids = (uint64_t *) buf;
	*idlen = st.len / sizeof(**ids);

	close_error = VOP_CLOSE(vp, mode, NULL, td);
	if (close_error != 0)
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);

	return (0);
}

static int
sls_read_slos_record(uint64_t oid, struct slskv_table *rectable,
    struct slskv_table *datatable)
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
	VOP_LOCK(vp, LK_EXCLUSIVE);
	if (error != 0) {
		SLS_DBG("getting record type failed with %d\n", error);
		return (error);
	}

	/* 
	 * If the record can hold data, "typecast" it to look like it only has 
	 * metadata. We will manually read the data later.
	 */
	if (sls_isdata(st.type)) {
		ret = sls_readdata_slos(vp, oid, rectable, datatable);
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
		SLS_DBG("error %d, could not close slos node", close_error);

	vput(vp);

	return (ret);
}

/* Reads in a record from the SLOS and saves it in the record table. */
int
sls_read_slos(uint64_t oid, struct slskv_table **rectablep,
    struct slskv_table **datatablep)
{
	struct slskv_table *rectable, *datatable;
	uint64_t *ids = NULL;
	size_t idlen;
	int error, i;

	/* Create the tables that hold the data and metadata of the checkpoint. 
	 * */
	error = slskv_create(&rectable);
	if (error != 0)
		return (error);

	error = slskv_create(&datatable);
	if (error != 0) {
		slskv_destroy(rectable);
		return (error);
	}

	/* 
	 * Read the manifest, get the record number and vnode numbers for the 
	 * checkpoint. 
	 */
	error = sls_read_slos_manifest(oid, &ids, &idlen);
	if (error != 0)
		goto error;

	for (i = 0; i < idlen; i++) {
		error = sls_read_slos_record(ids[i], rectable, datatable);
		if (error != 0)
			goto error;

	}

	*rectablep = rectable; 
	*datatablep = datatable;

	return (0);

error:
	free(ids, M_SLSMM);
	sls_free_rectable(rectable);
	sls_free_datatable(datatable);

	return error;
}

/* 
 * Traverse an object's resident pages, returning the maximum
 * length of data that is contiguous both in the object space
 * and in physical memory. We need both properties because the
 * data is contiguous on the disk if it is contiguous in the
 * object, and it does not need coalescing by the IO layer
 * if it is physically contiguous.
 */
static size_t
sls_contig_pages(vm_object_t obj, vm_page_t *page)
{
	vm_page_t prev, cur;
	size_t contig_len; 

	/* 
	 * At each step we have a pair of pages whose indices 
	 * the objexct and physical addresses are checked. 
	 */
	prev = *page;
	cur = TAILQ_NEXT(prev, listq);
	contig_len = pagesizes[prev->psind];
	sls_pages_grabbed += (pagesizes[prev->psind] / PAGE_SIZE);

	TAILQ_FOREACH_FROM(cur, &obj->memq, listq) {
		/* Pages need to be physically and logically contiguous. */
		if (prev->phys_addr + pagesizes[prev->psind] != cur->phys_addr ||
		    prev->pindex + OFF_TO_IDX(pagesizes[prev->psind]) != cur->pindex)
			break;

		/* 
		 * XXX Not sure we need this. Even if the run is gigantic, we do
		 * no copies, and the IO layer surely has a way of coping with 
		 * massive IOs.
		 */
		if (contig_len > sls_contig_limit)
			break;

		/* 
		 * Add the size of the "right" page to 
		 * the total, since it's contiguous.
		 */
		contig_len += pagesizes[cur->psind];
		prev = cur;
		sls_pages_grabbed += (pagesizes[cur->psind] / PAGE_SIZE);
	}

	/* Pass the new index into the page list to the caller. */
	*page = cur;

	return contig_len;
}

/*
 * Send len bytes of data, starting from physical page m, to vnode vp representing obj.
 */
static int
sls_writeobj_slos_sync(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len)
{
	struct iovec aiov;
	struct uio auio;
	vm_offset_t data;
	int error;

	/* Get the starting physical address of the pages. */
	/* XXX Only tested on amd64 */
	data = PHYS_TO_DMAP(m->phys_addr);

	/* Create the UIO for the disk. */
	aiov.iov_base = (void *) data;
	aiov.iov_len = len;

	/*
	 * The offset of the write adjusted because the first
	 * page-sized chunk holds the metadata of the VM object.
	 */
	slos_uioinit(&auio, PAGE_SIZE + IDX_TO_OFF(m->pindex),
	    UIO_WRITE, &aiov, 1);

	/* The write itself. */
	error = sls_doio(vp, &auio);
	if (error != 0)
		return (error);

	return (0);
}

/*
 * Get the pages of an object in the form of a
 * linked list of contiguous memory areas.
 *
 * This function is not inlined in order to be able to use DTrace on it.
 */
static int __attribute__ ((noinline))
sls_writeobj_slos(struct vnode *vp, vm_object_t obj)
{
	vm_page_t startpage, page;
	size_t contig_len;
	int error;

	/* Traverse the object's resident pages. */
	page = TAILQ_FIRST(&obj->memq);
	while (page != NULL) {
		/*
		 * Split the memory into contiguous chunks.
		 * By doing so, we can efficiently dump the
		 * data to the backend. The alternative
		 * would be sending out the data one page
		 * at a time, which would fragment the IOs
		 * and kill performance. The list of resident
		 * pages is being iterated in the call below.
		 */
		startpage = page;
		contig_len = sls_contig_pages(obj, &page);

		if (sls_async_slos)
			error = slos.slsfs_vmawrite(vp, obj, startpage, contig_len, NULL);
		else
			error = sls_writeobj_slos_sync(vp, obj, startpage, contig_len);
		if (error != 0)
			return (error);

		/* Have we fully traversed the list, or looped around? */
		if (page == NULL || startpage->pindex >= page->pindex)
			break;
	}

	return 0;
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

	/* Try to create the node, if not already there, and wrap it in a vnode. 
	 * */
	oid = rec->srec_id;
	error = slsfs_new_node(&slos, MAKEIMODE(VREG, S_IRWXU), &oid);
	if (error != 0)
		return (EINVAL);

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

	VOP_UNLOCK(vp, 0);
	error = VOP_IOCTL(vp, SLS_SET_RSTAT, &st, 0, NULL, td);
	VOP_LOCK(vp, LK_EXCLUSIVE);
	if (error != 0) {
		SLS_DBG("setting record type failed with %d\n", error);
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
			SLS_DBG("error %d, could not close SLSFS vnode\n", close_error);
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
	vm_object_t obj, newobj;
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
		vminfo = (struct slsvmobject *) sbuf_data(sb);
		obj = (vm_object_t) vminfo->id;
	}
#ifdef SLS_TEST
	else if (rec->srec_type == SLOSREC_TESTDATA) {
		datainfo = (struct data_info*) sbuf_data(sb);
		obj = (vm_object_t) datainfo->slsid;
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

	/* Get the shadow we created for the object from the table. */
	ret = slskv_find(objtable, (uint64_t) obj, (uintptr_t *) &newobj);
	if ((ret != 0) || (newobj == NULL))
		goto out;

	/* Checkpoint the object. */
	ret = sls_writeobj_slos(vp, obj);
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
	uint64_t slsid;
	int error;

	sb_manifest = sbuf_new_auto();

	KV_FOREACH_POP(sckpt_data->sckpt_rectable, slsid, rec) {
		/*
		 * VM object records are special, since we need to dump
		 * actual memory along with the metadata.
		 */
		if (sls_isdata(rec->srec_type))
			error = sls_writedata_slos(rec, sckpt_data->sckpt_objtable);
		else
			error = sls_writemeta_slos(rec, NULL, true);
		if (error != 0) {
			SLS_DBG("Writing the record failed with %d", error);
			sbuf_delete(sb_manifest);
			return (error);
		}

		/* Attach the new record to the checkpoint manifest. */
		error = sbuf_bcat(sb_manifest, &rec->srec_id, sizeof(rec->srec_id));
		if (error != 0) {
			sbuf_delete(sb_manifest);
			return (error);
		}

		/* XXX Free as we go. */
	}

	error = sls_write_slos_manifest(oid, sb_manifest);
	if (error != 0)
		return (error);


	sbuf_delete(sb_manifest);

	return (0);
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
	 * where we construct the meta_info object before copying over to the sbuf.
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
slstable_mkinfo_addrec(struct slskv_table *table, uint64_t slsid, meta_info *minfo, uint64_t type)
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
	error = slskv_add(table, slsid, (uint64_t) rec);
	if (error != 0) {
		free(rec, M_SLSMM);
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
slstable_mkinfo(struct slsckpt_data *sckpt_data, struct slsckpt_data *sckpt_tmpdata,
    vm_object_t obj)
{
	meta_info *minfo;
	uint64_t slsid;
	int error = 0;
	uint64_t type;

	type = (obj != NULL) ? SLOSREC_TESTDATA : SLOSREC_TESTMETA;
	slsid = (obj != NULL) ? (uint64_t) obj : random();

	/* Create the metadata info struct to be added. */
	error = slstable_mkinfo_meta(slsid, &minfo, type);
	if (error != 0)
		return (error);

	/* Add it to both tables. */
	error = slstable_mkinfo_addrec(sckpt_data->sckpt_rectable, slsid, minfo, type);
	if (error != 0)
		goto out;

	error = slstable_mkinfo_addrec(sckpt_tmpdata->sckpt_rectable, slsid, minfo, type);
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
	int error = 0;

	/* Find the sbuf that corresponds to the new element. */
	error = slskv_find(origtable, rec->srec_id, (uintptr_t *) &origrec);
	if (error != 0) {
		printf("ERROR: Retrieved element not found in the original table\n");
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
	minfo = (meta_info *) sbuf_data(rec->srec_sb);
	if (minfo == NULL) {
		return (EINVAL);
	}

	/* Unpack the original record from the sbuf. */
	origminfo = (meta_info *) sbuf_data(origrec->srec_sb);
	if (origminfo == NULL) {
		return (EINVAL);
	}

	/* Compare the original and retrieved records. They should be identical. */
	if (memcmp(origminfo->data, minfo->data, sizeof(*minfo))) {
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

	/* Map it into the kernel, fill it with data. */
	data = (char *) pmap_map(NULL, m->phys_addr,
		m->phys_addr + PAGE_SIZE,
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

	*objp = obj;

	return (0);
}

static int
slstable_mkdata(struct slsckpt_data *sckpt_data, struct slsckpt_data *sckpt_tmpdata,
    vm_object_t *objp)
{
	vm_object_t obj;
	int error;

	error = slstable_mkdata_object(&obj);
	if (error != 0)
		return (error);

	vm_object_reference(obj);
	error = slskv_add(sckpt_data->sckpt_objtable, (uint64_t) obj, (uintptr_t) obj);
	if (error != 0) {
		vm_object_deallocate(obj);
		return (error);
	}

	vm_object_reference(obj);
	error = slskv_add(sckpt_tmpdata->sckpt_objtable, (uint64_t) obj, (uintptr_t) obj);
	if (error != 0) {
		vm_object_deallocate(obj);
		return (error);
	}

	*objp = obj;
	return (0);
}

static int
slstable_testdata(struct slskv_table *datatable, struct slskv_table *objtable,
    struct sls_record *rec)
{
	struct slspagerun *pagerun, *last = NULL, *tmp;
	struct slspagerun *oldrun, *newrun;
	struct slsdata *newdata, olddata;
	uint8_t *oldpages, *newpages;
	vm_page_t page, startpage;
	size_t contig_len;
	vm_object_t obj;
	int error = 0;
	char *data;

	/* Bring in the restored list. */
	error = slskv_find(datatable, (uint64_t) rec->srec_id, (uintptr_t *) &newdata);
	if (error != 0) {
		printf("Could not find new data in the data table\n");
		return (error);
	}

	/* Find the object in the table. */
	error = slskv_find(objtable, rec->srec_id, (uintptr_t *) &obj);
	if (error != 0) {
		printf("Restored object not found\n");
		return (error);
	}
	slskv_del(objtable, rec->srec_id);

	LIST_INIT(&olddata);

	/* Traverse the list of pages of the object, like when dumping. */
	page = TAILQ_FIRST(&obj->memq);
	while (page != NULL) {
		/* Get the next contiguous chunk of pages*/
		startpage = page;
		contig_len = sls_contig_pages(obj, &page);

		/* Map the data in. */
		data = (char *) pmap_map(NULL, startpage->phys_addr, 
		    startpage->phys_addr + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);

		/* Create the list node. */
		pagerun = uma_zalloc(slspagerun_zone, M_WAITOK);
		pagerun->idx = startpage->pindex;
		pagerun->len = contig_len;
		pagerun->data = data;


		/*
		 * Insert the new element at the _tail_ of
		 * the list, and update the tail variable.
		 */
		LIST_INSERT_HEAD(&olddata, pagerun, next);

		last = pagerun;

		/* Have we fully traversed all pages, or looped around? */
		if (page == NULL || startpage->pindex >= page->pindex)
			break;
	}

	/* Compare the two lists. */
	for (;;) {
		/* If one of the lists is emptied out, we're done. */
		if (LIST_EMPTY(newdata) || LIST_EMPTY(&olddata))
			break;

		/* Pop the lists, compare the pageruns. */
		oldrun = LIST_FIRST(&olddata);
		LIST_REMOVE(oldrun, next);

		newrun = LIST_FIRST(newdata);
		LIST_REMOVE(newrun, next);

		if (oldrun->idx != newrun->idx) {
			printf("ERROR: Pagerun indices different in original and restored data\n");
			printf("Old index: %lx. New index: %lu.\n", oldrun->idx, newrun->idx);
			error = EINVAL;
			break;
		}

		if (oldrun->len != newrun->len) {
			printf("ERROR: Pagerun lengths different in original and restored data\n");
			printf("Old length: %lu. New length: %lu.\n", oldrun->len, newrun->len);
			error = EINVAL;
			break;
		}

		oldpages = (char *) oldrun->data;
		newpages = (char *) newrun->data;
		if (memcmp(newpages, oldpages, oldrun->len) != 0) {
			printf("ERROR: Old and new pageruns have different pages.\n");
			printf("First 32 bytes of old pages: %.32s\n", oldpages);
			printf("First 32 bytes of new pages: %.32s\n", newpages);
			break;
		}

		free(newrun->data, M_SLSMM);
		uma_zfree(slspagerun_zone, oldrun);
		uma_zfree(slspagerun_zone, newrun);
	}

	/* The lists must be of the same size, so they must be empty simultaneously. */
	if (!(LIST_EMPTY(newdata) && LIST_EMPTY(&olddata))) {
		printf("ERROR: Original and restored list have different sizes\n");
		error = EINVAL;
	}

	/* We don't need the pages anymore after this function. */
	LIST_FOREACH_SAFE(pagerun, &olddata, next, tmp) {
		LIST_REMOVE(pagerun, next);
		uma_zfree(slspagerun_zone, pagerun);
	}

	LIST_FOREACH_SAFE(pagerun, newdata, next, tmp) {
		LIST_REMOVE(pagerun, next);
		free(pagerun->data, M_SLSMM);
		uma_zfree(slspagerun_zone, pagerun);
	}

	vm_object_deallocate(obj);

	return error;
}

int
slstable_test(void)
{
	struct slskv_table *datatable = NULL, *rectable= NULL;
	struct slsckpt_data *sckpt_data = NULL, *sckpt_tmpdata = NULL;
	struct sls_record *rec;
	vm_object_t obj = NULL;
	uint64_t lost_elements;
	struct slskv_iter iter;
	struct sbuf *sb;
	uint64_t slsid;
	uint64_t type;
	int error, i;

	/*
	 * We create two checkpoints, one of which will be consumed in the
	 * writing process.
	 */
	error = slsckpt_create(&sckpt_data);
	if (error != 0) {
		SLS_ERROR(slsckpt_create, error);
		goto out;
	}

	error = slsckpt_create(&sckpt_tmpdata);
	if (error != 0) {
		SLS_ERROR(slsckpt_create, error);
		goto out;
	}

	/*
	 * Create the pure metadata objects. These are straightforward, because they
	 * are just binary blobs - they don't have actual user data that needs saving.
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

		/* For each object, create an sbuf, associate the two. */
		error = slstable_mkinfo(sckpt_data, sckpt_tmpdata, obj);
		if (error != 0) {
			vm_object_deallocate(obj);
			goto out;
		}

	}

	/* Write the data generated to the SLOS. */
	error = sls_write_slos(VNODE_ID, sckpt_tmpdata);
	if (error != 0) {
		SLS_ERROR(sls_write_slos, error);
		goto out;
	}
	slsckpt_destroy(sckpt_tmpdata);
	sckpt_tmpdata = NULL;

	/* Read the data back. */
	error = sls_read_slos(VNODE_ID, &rectable, &datatable);
	if (error != 0) {
		SLS_ERROR(sls_read_slos, error);
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
	KV_FOREACH(rectable, iter, slsid, rec) {
		switch (rec->srec_type) {
		case SLOSREC_TESTMETA:
			/* Test only the metadata, since there is no data. */
			error = slstable_testmeta(sckpt_data->sckpt_rectable, rec);
			if (error != 0) {
				KV_ABORT(iter);
				SLS_ERROR(slstable_testmeta, error);
				goto out;
			}

			break;

		case SLOSREC_TESTDATA:
			/*
			 * A data record also has metadata; test it separately.
			 */
			error = slstable_testmeta(sckpt_data->sckpt_rectable, rec);
			if (error != 0)
				goto out;

			/* Go check the data itself. */
			error = slstable_testdata(datatable, sckpt_data->sckpt_objtable,
			    rec);
			if (error != 0) {
				SLS_ERROR(slstable_testdata, error);
				goto out;
			}

			break;

		default:
			printf("ERROR: Invalid type found: %ld.\n", rec->srec_type);
			printf("Valid types: %x, %x\n", SLOSREC_TESTMETA, SLOSREC_TESTDATA);
			KV_ABORT(iter);
			goto out;

		}
	}

	lost_elements = 0;
	KV_FOREACH_POP(sckpt_data->sckpt_objtable, sb, type) {
		sbuf_delete(sb);
		lost_elements += 1;
	}

	if (lost_elements != 0) {
		printf("ERROR: Lost %lu elements between checkpoint and restore\n", lost_elements);
		goto out;
	}

	printf("Everything went OK.\n");

out:

	if (sckpt_data != NULL) {
		/* Pop and free. */
		slsckpt_destroy(sckpt_data);
	}

	/* The backup table has the same data as the original - all sbufs have been destroyed. */
	if (sckpt_tmpdata != NULL)
		slsckpt_destroy(sckpt_tmpdata);

	if (rectable!= NULL)
		sls_free_rectable(rectable);

	/*
	if (datatable != NULL)
		sls_free_datatable(datatable);
		*/

	/* XXX Clean up on disk after we're done. */

	return (error);
}

#endif /* SLS_TEST */ 
