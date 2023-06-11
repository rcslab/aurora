#include <sys/param.h>
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
#include "sls_message.h"
#include "sls_table.h"
#include "sls_vm.h"

static int
sls_write_socket_ckptstart(struct slspart *slsp, int sockfd)
{
	struct slsmsg_ckptstart *ckptmsg;
	union slsmsg msg;

	bzero(&msg, sizeof(msg));
	ckptmsg = (struct slsmsg_ckptstart *)&msg;

	*ckptmsg = (struct slsmsg_ckptstart) {
		.slsmsg_type = SLSMSG_CKPTSTART,
		.slsmsg_epoch = slsp->slsp_epoch - 1,
	};

	return (slsio_fdwrite(sockfd, (char *)&msg, sizeof(msg), NULL));
}

static int
sls_write_socket_ckptdone(int sockfd)
{
	struct slsmsg_ckptdone *donemsg;
	union slsmsg msg;

	bzero(&msg, sizeof(msg));
	donemsg = (struct slsmsg_ckptdone *)&msg;

	*donemsg = (struct slsmsg_ckptdone) {
		.slsmsg_type = SLSMSG_CKPTDONE,
	};

	return (slsio_fdwrite(sockfd, (char *)&msg, sizeof(msg), NULL));
}

static int
sls_write_socket_meta(int sockfd, struct sls_record *rec, size_t totallen)
{
	struct slsmsg_recmeta *metamsg;
	union slsmsg msg;
	int error;

	bzero(&msg, sizeof(msg));
	metamsg = (struct slsmsg_recmeta *)&msg;

	*metamsg = (struct slsmsg_recmeta) {
		.slsmsg_type = SLSMSG_RECMETA,
		.slsmsg_uuid = rec->srec_id,
		.slsmsg_metalen = sbuf_len(rec->srec_sb),
		.slsmsg_rectype = rec->srec_type,
		.slsmsg_totalsize = totallen,
	};

	error = slsio_fdwrite(sockfd, (char *)&msg, sizeof(msg), NULL);
	if (error != 0)
		return (error);

	return (slsio_fdwrite(
	    sockfd, sbuf_data(rec->srec_sb), sbuf_len(rec->srec_sb), NULL));
}

#include <sys/md5.h>
static void
md5_page(vm_page_t m)
{
	char digest[MD5_DIGEST_LENGTH];
	MD5_CTX md5sum;

	MD5Init(&md5sum);
	MD5Update(&md5sum, (void *)PHYS_TO_DMAP(m->phys_addr), PAGE_SIZE);
	MD5Final(digest, &md5sum);
	printf("Wrote %lx digest %lx\n", m->pindex, *(uint64_t *)digest);
}

static int
sls_write_socket_pages(int sockfd, vm_page_t m, size_t pagecnt)
{
	struct slsmsg_recpages *pagemsg;
	union slsmsg msg;
	int error;

	bzero(&msg, sizeof(msg));
	pagemsg = (struct slsmsg_recpages *)&msg;

	*pagemsg = (struct slsmsg_recpages) {
		.slsmsg_type = SLSMSG_RECPAGES,
		.slsmsg_len = PAGE_SIZE,
		.slsmsg_offset = (m->pindex + SLOS_OBJOFF) * PAGE_SIZE,
	};

	error = slsio_fdwrite(sockfd, (char *)&msg, sizeof(msg), NULL);
	if (error != 0)
		return (error);

	return (slsio_fdwrite(sockfd, (char *)PHYS_TO_DMAP(m->phys_addr),
	    pagecnt * PAGE_SIZE, NULL));
}

static int
sls_writedata_socket_pages(int sockfd, vm_object_t obj, bool *done)
{
	vm_pindex_t pindex;
	vm_page_t m;
	int error;

	*done = false;

	VM_OBJECT_WLOCK(obj);
	vm_object_pip_add(obj, 1);

	/*
	 * start from 0;
	 * keep going while physically and virtually contiguous;
	 * when not,
	 * break when we are done iterating.
	 */
	pindex = 0;
	while ((m = vm_page_find_least(obj, pindex)) != NULL) {
		KASSERT(m->object == obj,
		    ("page %p in object %p "
		     "associated with object %p",
			m, obj, m->object));
		KASSERT(pagesizes[m->psind] <= PAGE_SIZE,
		    ("dumping page %p with size %ld", m, pagesizes[m->psind]));

		m->oflags |= VPO_SWAPINPROG;
		VM_OBJECT_WUNLOCK(obj);

		error = sls_write_socket_pages(sockfd, m, 1);

		VM_OBJECT_WLOCK(obj);
		m->oflags &= ~VPO_SWAPINPROG;

		if (error != 0)
			break;

		pindex = m->pindex + 1;
	}

	KASSERT(m == NULL, ("not null"));
	vm_object_pip_add(obj, -1);
	VM_OBJECT_WUNLOCK(obj);

	*done = true;
	return (error);
}

static int
sls_writedata_socket(int sockfd, struct sls_record *rec)
{
	uint64_t totalpages, totalsize;
	struct slsvmobject *vminfo;
	vm_object_t obj;
	bool done;
	int error;

	KASSERT(rec->srec_type == SLOSREC_VMOBJ, ("not a data record"));

	/* Get the object itself, clean up the pointer when writing out. */
	vminfo = (struct slsvmobject *)sbuf_data(rec->srec_sb);
	obj = (vm_object_t)vminfo->objptr;
	vminfo->objptr = NULL;

	totalpages = (obj != NULL) ? obj->size + 1 : 1;
	totalsize = (totalpages + SLOS_OBJOFF) * PAGE_SIZE;

	error = sls_write_socket_meta(sockfd, rec, totalsize);
	if (error != 0)
		goto out;

	if (obj == NULL || !OBJT_ISANONYMOUS(obj))
		goto out;

	do {
		error = sls_writedata_socket_pages(sockfd, obj, &done);
		if (error != 0)
			goto out;
	} while (!done);

out:
	/* Release the reference this function holds. */
	vm_object_deallocate(obj);

	return (error);
}

static int
sls_write_socket_connect(struct slspart *slsp, int *sockfdp)
{
	struct thread *td = curthread;
	struct sockaddr_in sa;
	socklen_t alen;
	int sockfd;
	int error;

	memcpy(&alen, slsp->slsp_name, sizeof(alen));
	KASSERT(alen <= sizeof(sa), ("socket name too large"));
	if (alen > sizeof(sa)) {
		panic("%d %ld\n", alen, sizeof(sa));
		return (ENAMETOOLONG);
	}

	memcpy(&sa, &slsp->slsp_name[sizeof(alen)], alen);

	error = kern_socket(td, AF_INET, SOCK_STREAM, 0);
	if (error != 0) {
		panic("kern_socket error %d\n", error);
		return (error);
	}
	sockfd = td->td_retval[0];

	error = kern_connectat(td, AT_FDCWD, sockfd, (struct sockaddr *)&sa);
	if (error != 0) {
		panic("kern_connectat error %d\n", error);
		kern_close(td, sockfd);
		return (error);
	}

	*sockfdp = sockfd;
	return (0);
}

int
sls_write_socket(struct slspart *slsp, struct slsckpt_data *sckpt)
{
	struct thread *td = curthread;
	struct sls_record *rec = NULL;
	struct slskv_iter iter;
	uint64_t numids = 0;
	uint64_t slsid;
	int sockfd;
	int error;
	int ret;

	error = sls_write_socket_connect(slsp, &sockfd);
	if (error != 0)
		return (error);

	error = sls_write_socket_ckptstart(slsp, sockfd);
	if (error != 0)
		goto out;

	KV_FOREACH(sckpt->sckpt_rectable, iter, slsid, rec)
	{
		/* Create each record in parallel. */
		if (!sls_isdata(rec->srec_type))
			continue;

		KASSERT(slsid == rec->srec_id, ("record has wrong key"));

		error = sls_writedata_socket(sockfd, rec);
		if (error != 0)
			goto out;
	}

	/* XXX The + 1 is a gnarly hack to run the server and the client in the
	 * same machine. */
	rec = sls_getrecord_empty(slsp->slsp_oid + 1, SLOSREC_MANIFEST);
	if (rec == NULL) {
		error = ENOMEM;
		goto out;
	}

	numids = sbuf_len(sckpt->sckpt_dataid) / sizeof(uint64_t);
	error = sbuf_bcat(rec->srec_sb, &numids, sizeof(uint64_t));
	if (error != 0)
		goto out;

	error = sbuf_bcat(rec->srec_sb, sbuf_data(sckpt->sckpt_dataid),
	    sbuf_len(sckpt->sckpt_dataid));
	if (error != 0)
		goto out;

	error = sbuf_bcat(rec->srec_sb, sbuf_data(sckpt->sckpt_meta),
	    sbuf_len(sckpt->sckpt_meta));
	if (error != 0)
		goto out;

	sls_record_seal(rec);

	error = sls_write_socket_meta(sockfd, rec, sbuf_len(rec->srec_sb));
	if (error != 0)
		goto out;

	error = sls_write_socket_ckptdone(sockfd);
	if (error != 0)
		goto out;

out:
	if (rec != NULL)
		sls_record_destroy(rec);

	ret = kern_close(td, sockfd);
	if (ret != 0)
		printf("kern_close on socket failed with %d\n", ret);

	return (error);
}

int
sls_write_rcvdone(struct slspart *slsp)
{
	struct thread *td = curthread;
	struct slsmsg_done *donemsg;
	union slsmsg msg;
	int error;
	int ret;
	int fd;

	error = sls_write_socket_connect(slsp, &fd);
	if (error != 0)
		return (error);

	donemsg = (struct slsmsg_done *)&msg;
	donemsg->slsmsg_type = SLSMSG_DONE;

	error = slsio_fdwrite(fd, (char *)&msg, sizeof(msg), NULL);

	ret = kern_close(td, fd);

	return (error);
}
