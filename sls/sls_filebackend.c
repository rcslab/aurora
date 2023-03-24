#include <sys/param.h>
#include <sys/filio.h>
#include <sys/ktr.h>
#include <sys/lock.h>
#include <sys/proc.h>
#include <sys/rwlock.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

#include <machine/vmparam.h>

#include "debug.h"
#include "sls_data.h"
#include "sls_internal.h"
#include "sls_io.h"
#include "sls_table.h"
#include "sls_vm.h"

/*
 * XXX Write pages one at a time. Contiguous IOs require using UIOs as scatter/
 * gather arrays as designed. We can do this after we ensure correctness.
 */
static int
sls_writedata_file_pages(int fd, vm_object_t obj)
{
	vm_pindex_t pindex;
	int error = 0;
	vm_page_t m;
	off_t off;

	VM_OBJECT_WLOCK(obj);
	vm_object_pip_add(obj, 1);

	pindex = 0;
	while ((m = vm_page_find_least(obj, pindex)) != NULL) {

		KASSERT(m->object == obj,
		    ("page %p in object %p "
		     "associated with object %p",
			m, obj, m->object));
		KASSERT(pagesizes[m->psind] <= PAGE_SIZE,
		    ("dumping page %p with size %ld", m, pagesizes[m->psind]));

		off = (m->pindex + SLOS_OBJOFF) * PAGE_SIZE;

		m->oflags |= VPO_SWAPINPROG;
		VM_OBJECT_WUNLOCK(obj);

		error = slsio_fdwrite(
		    fd, (char *)PHYS_TO_DMAP(m->phys_addr), PAGE_SIZE, &off);

		VM_OBJECT_WLOCK(obj);
		m->oflags &= ~VPO_SWAPINPROG;

		if (error != 0)
			break;

		pindex += 1;
	}
	KASSERT(m == NULL, ("not null"));

	vm_object_pip_add(obj, -1);
	VM_OBJECT_WUNLOCK(obj);

	return (error);
}

static int
sls_writedata_file(
    int recfd, struct sls_record *rec, struct slsckpt_data *sckpt)
{
	struct slsvmobject *vminfo;
	vm_object_t obj;
	size_t off = 0;
	int error;

	/* Get the object itself, clean up the pointer when writing out. */
	vminfo = (struct slsvmobject *)sbuf_data(rec->srec_sb);
	obj = (vm_object_t)vminfo->objptr;
	vminfo->objptr = NULL;

	error = slsio_fdwrite(
	    recfd, sbuf_data(rec->srec_sb), sbuf_len(rec->srec_sb), &off);
	if (error != 0)
		goto out;

	if (obj == NULL || !OBJT_ISANONYMOUS(obj))
		goto out;

	KASSERT(obj->objid == rec->srec_id,
	    ("object and record have different OIDs"));

	error = sls_writedata_file_pages(recfd, obj);

	/*
	 * XXX We do not populate the prefault vector, because
	 * we are not currently using this for Metropolis.
	 */

out:
	/* Release the reference this function holds. */
	vm_object_deallocate(obj);

	return (error);
}

static int
sls_write_file_setup(struct slspart *slsp, char *fullpath, int *manfdp)
{
	struct thread *td = curthread;
	int error;

	/* Create the directory for the checkpoint. */
	snprintf(
	    fullpath, PATH_MAX, "%s/%lu", slsp->slsp_name, slsp->slsp_epoch);
	error = kern_mkdirat(td, AT_FDCWD, fullpath, UIO_SYSSPACE, S_IRWXU);
	if (error != 0)
		return (error);

	snprintf(fullpath, PATH_MAX, "%s/%lu/%lu", slsp->slsp_name,
	    slsp->slsp_epoch, slsp->slsp_oid);
	return (slsio_open_vfs(fullpath, manfdp));
}

int
sls_write_file(struct slspart *slsp, struct slsckpt_data *sckpt)
{
	struct thread *td = curthread;
	struct sls_record *rec;
	struct slskv_iter iter;
	int manfd, recfd;
	uint64_t numids;
	char *fullpath;
	uint64_t slsid;
	int error;
	int ret;

	fullpath = malloc(PATH_MAX, M_SLSMM, M_WAITOK);

	error = sls_write_file_setup(slsp, fullpath, &manfd);
	if (error != 0) {
		free(fullpath, M_SLSMM);
		return (error);
	}

	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{
		/* Create each record in parallel. */
		if (!sls_isdata(rec->srec_type))
			continue;

		KASSERT(
		    slsid == rec->srec_id, ("record is under the wrong key"));

		snprintf(fullpath, PATH_MAX, "%s/%lu/%lu", slsp->slsp_name,
		    slsp->slsp_epoch, slsid);
		error = slsio_open_vfs(fullpath, &recfd);
		if (error != 0)
			goto out;

		/* This call consumes the file descriptor. */
		error = sls_writedata_file(recfd, rec, sckpt);
		ret = kern_close(td, recfd);
		if (ret != 0)
			printf("ckpt data kern_close failed with %d\n", ret);

		if (error != 0)
			goto out;
	}

	numids = sbuf_len(sckpt->sckpt_dataid) / sizeof(uint64_t);
	error = slsio_fdwrite(manfd, (char *)&numids, sizeof(uint64_t), NULL);
	if (error != 0)
		goto out;

	/* Append the OID of the record to the manifest. */
	error = slsio_fdwrite(manfd, sbuf_data(sckpt->sckpt_dataid),
	    sbuf_len(sckpt->sckpt_dataid), NULL);
	if (error != 0)
		goto out;

	error = slsio_fdwrite(manfd, sbuf_data(sckpt->sckpt_meta),
	    sbuf_len(sckpt->sckpt_meta), NULL);

	/* XXX Add the current epoch as an extended attribute. */

out:
	free(fullpath, M_SLSMM);

	ret = kern_close(td, manfd);
	if (ret != 0)
		printf("ckpt man kern_close failed with %d\n", ret);

	return (error);
}

static int
sls_readdata_getfd(struct slspart *slsp, uint64_t oid, int *fdp)
{
	char *fullpath;
	int error;

	fullpath = malloc(PATH_MAX, M_SLSMM, M_WAITOK);

	/* XXX Again, find the epoch instead of this hackery. */
	snprintf(fullpath, PATH_MAX, "%s/%lu/%lu", slsp->slsp_name,
	    slsp->slsp_epoch - 1, oid);

	error = slsio_open_vfs(fullpath, fdp);
	free(fullpath, M_SLSMM);

	return (error);
}

static int
sls_readdata_file_record(
    struct slskv_table *rectable, uint64_t oid, struct slsvmobject *vminfo)
{
	struct sls_record *rec;
	int error;

	rec = sls_getrecord_empty(oid, SLOSREC_VMOBJ);
	error = sbuf_bcat(rec->srec_sb, vminfo, sizeof(*vminfo));
	if (error != 0) {
		sls_record_destroy(rec);
		return (error);
	}

	sls_record_seal(rec);

	error = slskv_add(rectable, oid, (uintptr_t)rec);
	if (error != 0) {
		sls_record_destroy(rec);
		return (EEXIST);
	}

	return (0);
}

static int
sls_readdata_file(int fd, vm_object_t obj)
{
	struct thread *td = curthread;
	off_t start, end;
	vm_page_t m;
	off_t off;
	int error;

	struct stat sb;
	error = kern_fstat(td, fd, &sb);
	if (error != 0)
		return (error);

	start = SLOS_OBJOFF * PAGE_SIZE;
	while (true) {
		/* ENXIO denotes there is no more data till EOF. */
		error = kern_ioctl(td, fd, FIOSEEKDATA, (caddr_t)&start);
		if (error == ENXIO) {
			error = 0;
			break;
		}
		if (error != 0)
			break;

		end = start;
		error = kern_ioctl(td, fd, FIOSEEKHOLE, (caddr_t)&end);
		if (error != 0)
			break;

		KASSERT(start % PAGE_SIZE == 0,
		    ("unaligned read start %ld", start));
		KASSERT(end % PAGE_SIZE == 0, ("unaligned read end %ld", end));

		/* Roll start forward depending on the contiguity. */
		for (; start < end; start += PAGE_SIZE) {
			VM_OBJECT_WLOCK(obj);
			vm_page_grab_pages(obj,
			    (start / PAGE_SIZE) - SLOS_OBJOFF, VM_ALLOC_NORMAL,
			    &m, 1);

			m->valid = 0;
			m->oflags |= VPO_SWAPINPROG;

			vm_object_pip_add(obj, 1);
			VM_OBJECT_WUNLOCK(obj);

			off = start;
			slsio_fdread(fd, (char *)PHYS_TO_DMAP(m->phys_addr),
			    PAGE_SIZE, &off);

			m->valid = VM_PAGE_BITS_ALL;
			m->oflags &= ~VPO_SWAPINPROG;
			vm_page_xunbusy(m);

			VM_OBJECT_WLOCK(obj);
			/*
			 * The underlying FS might add zero blocks to the file
			 * because of its allocation size. These zero pages
			 * might interfere with execution in case the object is
			 * shadowing a parent which actually backs the zeroed
			 * out page with data. In that case the zeroed out page
			 * prevents shadowing and probably leads to a crash.
			 * Check if a page is zeroed out, assume that this is
			 * due to the underlying file system, and remove it.
			 *
			 * XXX Actually not even that is correct, what if the
			 * zeroes in the shadow are generated by the
			 * application? This system of storing checkpoints is
			 * imperfect in this regard.
			 */
			if (!memcmp((void *)PHYS_TO_DMAP(m->phys_addr),
				zero_region, PAGE_SIZE)) {
				vm_object_page_remove(
				    obj, m->pindex, m->pindex + 1, 0);
			}

			vm_object_pip_add(obj, -1);
			VM_OBJECT_WUNLOCK(obj);
		}

		start = end;
	}

	return (error);
}

static int
sls_read_file_datarec(struct slspart *slsp, uint64_t oid,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	struct thread *td = curthread;
	struct slsvmobject vminfo;
	vm_object_t obj;
	int error;
	int ret;
	int fd;

	error = sls_readdata_getfd(slsp, oid, &fd);
	if (error != 0)
		return (error);

	error = slsio_fdread(fd, (char *)&vminfo, sizeof(vminfo), NULL);
	if (error != 0)
		goto out;
	vminfo.objptr = NULL;

	error = sls_readdata_file_record(rectable, oid, &vminfo);
	if (error != 0)
		goto out;

	if ((vminfo.type != OBJT_DEFAULT) && (vminfo.type != OBJT_SWAP))
		goto out;

	obj = vm_pager_allocate(OBJT_DEFAULT, NULL, IDX_TO_OFF(vminfo.size),
	    VM_PROT_DEFAULT, 0, NULL);
	obj->objid = oid;

	error = slskv_add(objtable, oid, (uintptr_t)obj);
	if (error != 0) {
		vm_object_deallocate(obj);
		goto out;
	}

	/*
	 * We have to eagerly read in the data here, because we are not
	 * backing the restored partition with the on-disk data.
	 */
	error = sls_readdata_file(fd, obj);

out:
	ret = kern_close(td, fd);
	if (ret != 0)
		printf("kern_close on data failed with %d\n", ret);

	return (error);
}

static int
sls_read_file_datarec_all(struct slspart *slsp, char **bufp, size_t *buflenp,
    struct slskv_table *rectable, struct slskv_table *objtable)
{
	size_t original_buflen, data_buflen, data_idlen;
	uint64_t *data_ids;
	char *buf;
	int error;
	int i;

	original_buflen = *buflenp;
	buf = *bufp;

	/* Get the length of the original data OID array. */
	data_idlen = *(uint64_t *)buf;
	buf += sizeof(data_idlen);
	original_buflen -= sizeof(data_idlen);

	/* Now that we have the length, we can read the array itself. */
	data_ids = (uint64_t *)buf;
	data_buflen = data_idlen * sizeof(*data_ids);

	KASSERT(data_buflen < original_buflen,
	    ("VM data array larger than the buffer itself"));

	for (i = 0; i < data_idlen; i++) {
		error = sls_read_file_datarec(
		    slsp, data_ids[i], rectable, objtable);
		if (error != 0)
			return (error);
	}

	*bufp = &buf[data_buflen];
	*buflenp = original_buflen - data_buflen;

	return (0);
}

int
sls_read_file(struct slspart *slsp, struct slsckpt_data **sckptp,
    struct slskv_table *objtable)
{
	struct thread *td = curthread;
	struct slsckpt_data *sckpt;
	char *buf = NULL, *curbuf;
	struct stat sb;
	char *fullpath;
	size_t buflen;
	int manfd;
	int error;
	int ret;

	sckpt = slsckpt_alloc(&slsp->slsp_attr);
	if (sckpt == NULL)
		return (ENOMEM);

	SDT_PROBE1(sls, , sls_rest, , "Allocating checkpoint");

	fullpath = malloc(PATH_MAX, M_SLSMM, M_WAITOK);

	/* XXX Find the epoch by querying the folder, not by decrementing 1. */
	snprintf(fullpath, PATH_MAX, "%s/%lu/%lu", slsp->slsp_name,
	    slsp->slsp_epoch - 1, slsp->slsp_oid);
	error = slsio_open_vfs(fullpath, &manfd);
	if (error != 0)
		goto error;

	error = kern_fstat(td, manfd, &sb);
	if (error != 0) {
		ret = kern_close(td, manfd);
		if (ret != 0)
			printf("kern close on manifest failed with %d\n", ret);
		goto error;
	}

	buf = malloc(sb.st_size, M_SLSMM, M_WAITOK);

	error = slsio_fdread(manfd, buf, sb.st_size, NULL);

	ret = kern_close(td, manfd);
	if (ret != 0)
		printf("kern close on manifest failed with %d\n", ret);

	if (error != 0)
		goto error;

	SDT_PROBE1(sls, , sls_rest, , "Getting manifest");

	/* Use curbuf as a running index into the original buffer. */
	curbuf = buf;
	buflen = sb.st_size;

	/* Extract the list of VM records, return the array of metadata. */
	error = sls_read_file_datarec_all(
	    slsp, &curbuf, &buflen, sckpt->sckpt_rectable, objtable);
	if (error != 0)
		goto error;

	SDT_PROBE1(sls, , sls_rest, , "Getting data");

	/* Extract all metadata records. */
	error = sls_readmeta(curbuf, buflen, sckpt->sckpt_rectable);
	if (error != 0) {
		DEBUG1("%s: reading the metadata failed\n", __func__);
		goto error;
	}

	SDT_PROBE1(sls, , sls_rest, , "Getting metadata");

	free(fullpath, M_SLSMM);
	free(buf, M_SLSMM);

	*sckptp = sckpt;

	return (0);

error:
	free(fullpath, M_SLSMM);
	slsckpt_drop(sckpt);

	return (error);
}
