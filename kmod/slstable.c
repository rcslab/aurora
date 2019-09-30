#include <sys/types.h>

#include <sys/conf.h>
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

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <sls_data.h>
#include <slos.h>

#include "sls.h"
#include "slskv.h"
#include "slsmm.h"
#include "slstable.h"

#ifdef SLS_TEST
#include "slstable_test.h"
#endif /* SLS_TEST */


uma_zone_t slspagerun_zone = NULL;

/* 
 * A list of info structs which can hold data, 
 * along with the size of their metadata. 
 */
static uint64_t sls_datastructs[][2] =  { 
    { SLOSREC_VMOBJ, sizeof(struct vm_object_info) }, 
#ifdef SLS_TEST
    { SLOSREC_TESTDATA, sizeof(struct data_info) }, 
#endif /* SLS_TEST */
};

/* 
 * Find the size for the metadata of the data-holding struct. 
 * If the type cannot hold data, return an error. 
 */
static int
sls_datasizes(uint64_t type, uint64_t *size)
{
	int i;

	for (i = 0; i < sizeof(sls_datastructs) / (sizeof(*sls_datastructs)); i++) {
	    if (sls_datastructs[i][0] == type) {
		*size = sls_datastructs[i][1];
		return 0;
	    }
	}

	/* The type cannot hold data. */

	return EINVAL;
}

/*
 * Reads in a metadata record representing one or more SLS info structs.
 */
static int
sls_readmeta_slos(struct slos_vnode *vp, uint64_t rno, struct slos_rstat stat,
	struct slskv_table *table, void **recordp)
{
	struct iovec aiov;
	struct uio auio;
	void *record;
	int error;

	record = malloc(stat.len, M_SLSMM, M_WAITOK);

	/* Create the UIO for the disk. */
	aiov.iov_base = record;
	aiov.iov_len = stat.len;
	slos_uioinit(&auio, 0, UIO_READ, &aiov, 1);

	/* Keep reading until we get all the info. */
	while (auio.uio_resid > 0) {
	    error = slos_rread(vp, rno, &auio);
	    if (error != 0) {
		free(record, M_SLSMM);
		return error;
	    }
	}

	/* Add the record to the table to be parsed into info structs later. */
	error = slskv_add(table, (uint64_t) record, (uintptr_t) stat.type);
	if (error != 0) {
	    slskv_del(table, (uint64_t) record);
	    free(record, M_SLSMM);
	    return error;
	}

	*recordp = record;

	return 0;
}

/* 
 * Reads in a sparse record representing a VM object,
 * and stores it as a linked list of page runs. 
 */
static int
sls_readdata_slos(struct slos_vnode *vp, uint64_t rno, struct slos_rstat stat,
	struct slskv_table *metatable, struct slskv_table *datatable)
	
{
	struct slspagerun *pagerun;
	struct slsdata *data;
	void *record, *pages;
	struct iovec aiov;
	struct uio auio;
	uint64_t offset;
	uint64_t len;
	int error;

	/* 
	 * First read the VM object metadata. The data pages get read separately.
	 */
	
	error = sls_readmeta_slos(vp, rno, stat, metatable, &record);
	if (error != 0)
	    return error;

	/* 
	 * The data pages themselves will be added to the data table,
	 * represented as a linked list of page runs. We use the record's
	 * location in memory as the key for the data.
	 */

	data = malloc(sizeof(*data), M_SLSMM, M_WAITOK);
	LIST_INIT(data);

	error = slskv_add(datatable, (uint64_t) record, (uintptr_t) data);
	if (error != 0)
	    return error;


	/* We start from the end of the VM object metadata. */
	offset = sizeof(struct vm_object_info);
	for (;;) {

	    /* Seek the next extent in the record. */
	    error = slos_rseek(vp, rno, offset, SREC_SEEKRIGHT, &offset, &len);
	    if (error != 0) {
		printf("Seek failed\n");
		break;
	    }

	    /* If we get EOF, we're done. */
	    if (len == SREC_SEEKEOF)
		break;

	    /* Otherwise allocate a buffer for the data. */
	    pages = malloc(len, M_SLSMM, M_WAITOK);

	    /* Create the UIO for the disk. */
	    aiov.iov_base = pages;
	    aiov.iov_len = len;
	    slos_uioinit(&auio, offset, UIO_READ, &aiov, 1);

	    /* The read itself. */
	    while (auio.uio_resid > 0) {
		error = slos_rread(vp, rno, &auio);
		if (error != 0)
		    return error;
	    }


	    /* Add the new pagerun to the list. */
	    pagerun = uma_zalloc(slspagerun_zone, M_WAITOK);
	    /* 
	     * The pages start from the 2nd 4K block in the record. 
	     * Moreover, the index of the pages in the object 
	     * correspond to page-sized chunks.
	     */
	    pagerun->idx = OFF_TO_IDX(offset) - 1;
	    pagerun->len = len;
	    pagerun->data = pages;

	    /* Increment the offset by the amount of bytes read. */
	    offset += len;

	    LIST_INSERT_HEAD(data, pagerun, next);

	}

	return 0;
}

/*
 * Reads in a record from the SLOS and saves it in
 * the record table.
 */
int
sls_read_slos(struct slos_vnode *vp, struct slskv_table **metatablep,
	struct slskv_table **datatablep)
{
	struct slskv_table *metatable, *datatable;
	struct slspagerun *pagerun, *tmp;
	struct slos_rstat stat;
	struct slsdata data;
	uint64_t rno, type;
	void *record;
	int error;


	/* Create the tables that hold the data and metadata of the checkpoint. */
	error = slskv_create(&metatable, SLSKV_NOREPLACE, SLSKV_VALNUM);
	if (error != 0)
	    return error;

	error = slskv_create(&datatable, SLSKV_NOREPLACE, SLSKV_VALNUM);
	if (error != 0) {
	    slskv_destroy(metatable);
	    return error;
	}

	/* ------------ SLOS-Specific Part ------------ */

	/* Traverse the whole vnode, grabbing all records. */
	error = slos_firstrno(vp, &rno);
	if (error != 0)
	    goto error;

	/* 
	 * Every time we call readdata/readmeta we add the read
	 * SLOS record into a table to be parsed later.
	 */
	do {
	    error = slos_rstat(vp, rno, &stat);
	    if (error != 0)
		goto error;

	    /* 
	     * If the record can hold data,  "typecast" it to look like 
	     * it only has metadata. We will manually read the data later.
	     */
	    if (sls_datasizes(stat.type, &stat.len) != 0)
		error = sls_readmeta_slos(vp, rno, stat, metatable, &record);
	    else
		error = sls_readdata_slos(vp, rno, stat, metatable, datatable);

	    if (error != 0)
		goto error;

	} while(slos_nextrno(vp, &rno) == 0);

	/* ------------ End SLOS-Specific Part ------------ */

	*metatablep = metatable;
	*datatablep = datatable;

	return 0;

error:

	while (slskv_pop(metatable, (uint64_t *) &record, (uintptr_t *) &type) == 0)
	    free(record, M_SLSMM);

	while (slskv_pop(datatable, (uint64_t *) &record, (uintptr_t *) &data) == 0) {
	    LIST_FOREACH_SAFE(pagerun, &data, next, tmp) {
		free(pagerun->data, M_SLSMM);
		free(pagerun, M_SLSMM);
	    }
	}

	slskv_destroy(metatable);
	slskv_destroy(datatable);

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
	}

	/* Pass the new index into the page list to the caller. */
	*page = cur;

	return contig_len;
}

/* 
 * Get the pages of an object in the form of a 
 * linked list of contiguous memory areas.
 */
static int
sls_objdata(struct slos_vnode *vp, uint64_t rno, vm_object_t obj)
{
	vm_page_t startpage, page;
	size_t contig_len;
	vm_offset_t data;
	struct iovec aiov;
	struct uio auio;
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

	    /* Map the process' data into the kernel. */
	    data = pmap_map(NULL, startpage->phys_addr, 
		    startpage->phys_addr + contig_len, 
		    VM_PROT_READ | VM_PROT_WRITE);
	    if (data == 0)
		return ENOMEM;

	    /* ------------ SLOS-Specific Part ------------ */

	    /* Create the UIO for the disk. */
	    aiov.iov_base = (void *) data;
	    aiov.iov_len = contig_len;

	    /* 
	     * The offset of the write adjusted because the first
	     * page-sized chunk holds the metadata of the VM object.
	     */
	    slos_uioinit(&auio, PAGE_SIZE + IDX_TO_OFF(startpage->pindex), 
		    UIO_WRITE, &aiov, 1);

	    /* The write itself. */
	    while (auio.uio_resid > 0) {
		error = slos_rwrite(vp, rno, &auio);
		if (error != 0)
		    return error;
	    }

	    /* ------------ End SLOS-Specific Part ------------ */

	    /* XXX Do we need pmap_delete or something of the sort? */

	    /* Have we fully traversed the list, or looped around? */
	    if (page == NULL || startpage->pindex >= page->pindex)
		break;
	}

	if (error != 0)
	    return error;

	return 0;
}

/* 
 * Creates a record in the SLOS with the metadata held in the sbuf. 
 * The record is contiguous, and only has data in the beginning.
 */
static int
sls_writemeta_slos(struct slos_vnode *vp, struct sbuf *sb, uint64_t type, uint64_t *rnop)
{
	uint64_t rno;
	struct iovec aiov;
	struct uio auio;
	void *record;
	size_t len;
	int error;

	error = sbuf_finish(sb);
	if (error != 0)
	    return error;

	record = sbuf_data(sb);
	len = sbuf_len(sb);

	error = slos_rcreate(vp, type, &rno);
	if (error != 0)
	    return error;

	/* Create the UIO for the disk. */
	aiov.iov_base = record;
	aiov.iov_len = len;
	slos_uioinit(&auio, 0, UIO_WRITE, &aiov, 1);

	/* Keep reading until we get all the info. */
	while (auio.uio_resid > 0) {
	    error = slos_rwrite(vp, rno, &auio);
	    if (error != 0) {
		free(record, M_SLSMM);
		return error;
	    }
	}
	
	/* Push the record number of the new data to the caller. */
	*rnop = rno;

	return 0;
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
sls_writedata_slos(struct slos_vnode *vp, struct sbuf *sb, uint64_t type)
{
	struct vm_object_info *info;
	vm_object_t obj;
	uint64_t rno;
	int error;

	error = sls_writemeta_slos(vp, sb, type, &rno);
	if (error != 0)
	    return error;

	/* 
	 * We know that the sbuf holds a vm_object_info struct,
	 * that's why we are in this function in the first place.
	 */
	info = (struct vm_object_info *) sbuf_data(sb);

	/* 
	 * The ID of the info struct and the in-memory pointer
	 * are identical at checkpoint time, so we use it to
	 * retrieve the object and grab its data.
	 */
	obj = (vm_object_t) info->slsid;

	/* Send the object's data to the same record as the metadata. */
	error = sls_objdata(vp, rno, obj);
	if (error != 0)
	    return error;

	return 0;
}

/*
 * Dumps a table of records into the SLOS. The records of VM objects
 * are special cases, since they are sparse and hold all pages that
 * need to be sent to the SLOS.
 */
int
sls_write_slos(struct slos_vnode *vp, struct slskv_table *table)
{
	struct sbuf *sb;
	uint64_t type, size;
	uint64_t rno;
	int error;

	while (slskv_pop(table, (uint64_t *) &sb, (uintptr_t *) &type) == 0) {
	    /* 
	     * VM object records are special, since we need to dump 
	     * actual memory along with the metadata.
	     */

	    /* ------------ SLOS-Specific Part ------------ */

	    if (sls_datasizes(type, &size) != 0)
		error = sls_writemeta_slos(vp, sb, type, &rno);
	    else 
		error = sls_writedata_slos(vp, sb, type);

	    if (error != 0)
		return error;


	    /* ------------ End SLOS-Specific Part ------------ */
	}

	return 0;
}


/* ---------------------- TESTING ---------------------- */

#ifdef SLS_TEST

/*
 * Create a VM object and add data to it
 */
static int
slstable_mkobj(vm_object_t *objp)
{
	vm_object_t obj;
	vm_page_t page;
	uint8_t *data;
	int i, j;

	/* 
	 * Create the object to be dumped and restored; 
	 * we don't need to insert it anywhere.
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

	    VM_OBJECT_WLOCK(obj);

	    /* 
	     * Get a new page for the object and fill it with data. 
	     * This is the same procedure as when restoring the pages
	     * of a process, except that here we fill them with random data.
	     */
	    page = vm_page_alloc(obj, i, VM_ALLOC_NORMAL);
	    if (page == NULL) {
		/* If we get an error here, we just return what we have. */
		VM_OBJECT_WUNLOCK(obj);
		break;
	    }

	    page->valid = VM_PAGE_BITS_ALL;

	    VM_OBJECT_WUNLOCK(obj);

	    /* Map it into the kernel, fill it with data. */
	    data = (char *) pmap_map(NULL, page->phys_addr, 
		    page->phys_addr + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);

	    for (j = 0; j < PAGE_SIZE; j++)
		data[j] = 'a' + (random() % ('z' - 'a'));

	}

	*objp = obj;

	return 0;
}

/*
 * Create an info struct and insert it to the tables used for bookkeeping.
 * Note that this function only creates meta_info structs, but the two
 * are structurally equivalent.
 */
static int
slstable_mkinfo(struct slskv_table *origtable, struct slskv_table *backuptable, 
	uint64_t slsid, uint64_t type) 
{
	struct sbuf *sb, *sbbak;
	meta_info *minfo;
	int error = 0;
	int i;

	/* 
	 * This buffer doesn't get inserted to any table, however it is
	 * where we construct the meta_info object before copying over to the sbuf.
	 */
	minfo = malloc(sizeof(*minfo), M_SLSMM, M_WAITOK);

	sb = sbuf_new_auto();
	sbbak = sbuf_new_auto();

	/* Set up the metadata struct, including random data. */
	minfo->magic = (type == SLOSREC_TESTMETA) ? META_MAGIC : DATA_MAGIC;
	/* 
	 * The SLS ID is inconsequential for meta nodes, but very important
	 * for data, since it holds the address of the VM object.
	 */
	if (slsid == 0)
	    minfo->slsid = (uint64_t) minfo;
	else 
	    minfo->slsid = slsid;

	minfo->sbid = (uint64_t) sb;
	for (i = 0; i < INFO_SIZE; i++)
	    minfo->data[i] = 'a' + random() % ('z' - 'a');


	/* Copy over to the sbuf. */
	error = sbuf_bcpy(sb, (void *) minfo, sizeof(*minfo));
	if (error != 0)
	    goto out;

	/* Copy over to the sbuf. */
	error = sbuf_bcpy(sbbak, (void *) minfo, sizeof(*minfo));
	if (error != 0)
	    goto out;


	/* Add the sbuf to the "before" table. */
	error = slskv_add(origtable, (uint64_t) sb, type);
	if (error != 0)
	    goto out;

	/* Add the sbuf to the "before" table. */
	error = slskv_add(backuptable, (uint64_t) sbbak, type);
	if (error != 0)
	    goto out;

out:

	free(minfo, M_SLSMM);

	return error;
}

/* 
 * Test the retrieved metadata by checking it against the 
 * original data stored. Also possibly give back the VM
 * object holding the entry's data.
 */
static int
slstable_testmeta(struct slskv_table *origtable, 
	meta_info *record, uint64_t type)
{
	struct sbuf *sb = NULL;
	meta_info *origrecord; 
	uint64_t origtype;
	int error = 0;

	/* Find the sbuf that corresponds to the new element. */
	error = slskv_find(origtable, record->sbid, (uintptr_t *) &origtype);
	if (error != 0) {
	    printf("ERROR: Retrieved element not found in the original table\n");
	    goto out;
	}

	/* 
	 * Check whether the type is correct. This is an introductory test - 
	 * if it fails, we have probably read garbage.
	 */
	if (origtype != type) {
	    error = EINVAL;
	    printf("ERROR: Original type is %ld, type found is %ld\n", origtype, type);
	    goto out;
	}

	/* 
	 * Remove the element from the original table. This matters 
	 * when we look into whether all saved records were restored.
	 */
	slskv_del(origtable, record->sbid);

	sb = (struct sbuf *) record->sbid;
	error = sbuf_finish(sb);
	if (error != 0)
	    goto out;

	/* Unpack the original record from the sbuf. */
	origrecord = (meta_info *) sbuf_data(sb);
	if (origrecord == NULL) {
	    error = EINVAL;
	    goto out;
	}

	/* Compare the original and retrieved records. They should be identical. */
	if (memcmp(origrecord, record, sizeof(*origrecord))) {
	    printf("ERROR: Retrieved record differs from the original\n");
	    printf("Original data: %.*s\n", INFO_SIZE, origrecord->data);
	    printf("Retrieved data: %.*s\n", INFO_SIZE, record->data);
	    error = EINVAL;
	}

out:

	if (sb != NULL)
	    sbuf_delete(sb);

	return error;
}

static int
slstable_testdata(struct slskv_table *datatable, vm_object_t obj, meta_info *record)
{
	struct slspagerun *pagerun, *last = NULL, *tmp;
	struct slspagerun *oldrun, *newrun;
	struct slsdata *newdata, olddata;
	uint8_t *oldpages, *newpages;
	vm_page_t page, startpage;
	size_t contig_len;
	int error = 0;
	char *data;

	LIST_INIT(&olddata);

	/* 
	 * Traverse the list of pages of the object, 
	 * similar to the way we do it when dumping. 
	 */
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

	/* Bring in the restored list. */
	error = slskv_find(datatable, (uint64_t) record, (uintptr_t *) &newdata);
	if (error != 0) {
	    printf("ERROR: Restored data for VM object metadata not found\n");
	    goto out;
	}

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
		printf("Old index: %lu. New index: %lu.\n", oldrun->idx, newrun->idx);
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


out:
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

	return error;
}

int
slstable_test(void)
{
	struct slos_vnode *vp = NULL;
	struct slskv_table *origtable = NULL, *backuptable = NULL;
	struct slskv_table *datatable = NULL, *metatable = NULL;
	struct slspagerun *pagerun, *tmppagerun;
	struct slsdata *slsdata;
	uint64_t lost_elements;
	vm_object_t obj = NULL;
	meta_info *record;
	struct sbuf *sb;
	int sloserror;
	uint64_t type;
	int error, i;

	/* Create a vnode in the SLOS for testing. */
	error = slos_icreate(&slos, VNODE_ID);
	if (error != 0)
	    return error;

	vp = slos_iopen(&slos, VNODE_ID);
	if (vp == NULL)
	    goto out;

	/* 
	 * The table with the original info structs, plays the role
	 * of the actual table that is passed from the upper layers.
	 */
	error = slskv_create(&origtable, SLSKV_NOREPLACE, SLSKV_VALNUM);
	if (error != 0)
	    goto out;

	/*
	 * A backup table for the original data. When writing the data out,
	 * we pop the elements of the table in the process, and so we cannot
	 * use the table after restoring to compare. For this reason, we 
	 * create here a _second_ table, which will be passed to - and
	 * destroyed by - slstable_write.
	 */
	error = slskv_create(&backuptable, SLSKV_NOREPLACE, SLSKV_VALNUM);
	if (error != 0)
	    goto out;


	/* 
	 * Create the pure metadata objects. These are straightforward, because they
	 * are just binary blobs - they don't have actual user data that needs saving.
	 */
	for (i = 0; i < META_INFOS; i++) {
	    error = slstable_mkinfo(origtable, backuptable, 0, SLOSREC_TESTMETA);
	    if (error != 0)
		goto out;
	}


	for (i = 0; i < DATA_INFOS; i++) {
	    /* Create the data-holding objects. */
	    error = slstable_mkobj(&obj);
	    if (error != 0)
		goto out;

	    /* For each object, create an sbuf, associate the two. */
	    error = slstable_mkinfo(origtable, backuptable, (uint64_t) obj, SLOSREC_TESTDATA);
	    if (error != 0)
		goto out;

	}

	/* Write the data generated to the SLOS. */
	error = sls_write_slos(vp, backuptable);
	if (error != 0)
	    goto out;

	/* Read the data back. */
	error = sls_read_slos(vp, &metatable, &datatable);
	if (error != 0)
	    return error;

	/* 
	 * Pop all entries in the metadata table. For each entry we conduct a 
	 * number of tests, including:
	 * - Is the type as expected?
	 * - Does the element correspond to an sbuf in the original table?
	 * - Is the struct identical to the one in the original table?
	 * - If the entry is a data entry, also check its data.
	 */
	while (slskv_pop(metatable, (uint64_t *) &record, (uintptr_t *) &type) == 0) {

	    switch (type) {
	    case SLOSREC_TESTMETA:
		/* Test only the metadata, since there is no data. */
		error = slstable_testmeta(origtable, record, type);
		if (error != 0)
		    goto out;

		break;

	    case SLOSREC_TESTDATA:
		/* A data record also has metadata; test it separately. */
		error = slstable_testmeta(origtable, record, type);
		if (error != 0)
		    goto out;

		/* Go check the data itself. */
		error = slstable_testdata(datatable, 
			(vm_object_t) record->slsid, record);
		if (error != 0)
		    goto out;

		break;

	    default:
		printf("ERROR: Invalid type found: %ld.\n", type);
		printf("Valid types: %x, %x\n", SLOSREC_TESTMETA, SLOSREC_TESTDATA);
		goto out;

	    }

	    /* The record isn't needed anymore, so we free it. */
	    free(record, M_SLSMM);
	}

	lost_elements = 0;
	while (slskv_pop(origtable, (uint64_t *) &sb, (uintptr_t *) &type) == 0) {
	    sbuf_delete(sb);
	    lost_elements += 1;
	}

	if (lost_elements != 0) {
	    printf("ERROR: Lost %lu elements between checkpoint and restore\n", lost_elements);
	    goto out;
	}

	printf("Everything went OK.\n");
out:
	
	if (origtable != NULL) {
	    while (slskv_pop(origtable, (uint64_t *) &sb, (uintptr_t *) &type) == 0)
		sbuf_delete(sb);

	    slskv_destroy(origtable);
	}

	/* The backup table has the same data as the original - all sbufs have been destroyed. */
	if (backuptable != NULL)
	    slskv_destroy(backuptable);
	
	if (metatable != NULL) {
	    while (slskv_pop(metatable, (uint64_t *) &record, (uintptr_t *) &type) == 0)
		free(record, M_SLSMM);

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

	if (vp != NULL) {
	    sloserror = slos_iclose(&slos, vp);
	    if (sloserror != 0) {
		printf("ERROR: slos_iclose failed\n");
		return error;
	    }

	    sloserror = slos_iremove(&slos, VNODE_ID);
	    if (sloserror != 0) {
		printf("ERROR: slos_iremove failed with %d\n", error);
		return error;
	    }
	}

	return error;
}

#endif /* SLS_TEST */

/* --------------------------------------------*/
/* XXX PARSING after we are done thoroughly testing the code above */

#if 0
/*
 * Parse the data read from the backend into SLS info structs.
 */
int
sls_parsedata(struct slskv_table *rectable, struct slskv_table *infotable)
{
	void *record;
	uint64_t type;
    
    
	while (slskv_pop(rectable, (uint64_t *) &record, (uintptr_t *) &type) == 0) {
	    switch (type) {
	    case SLOSREC_PROC:
		/* The record holds a process and all its threads. */
		break;
	    case SLOSREC_MEM:
		/* The record holds a vmspace and its entries. */
		break;
	    case SLOSREC_MEMOBJT:
		/* 
		 * The record holds a VM object and its pages.
		 */
		break;
	    case SLOSREC_FDESC:
		/*
		 * The record holds a file descriptor and its associated data.
		 */
		break;

	    }

	}
}



#endif
