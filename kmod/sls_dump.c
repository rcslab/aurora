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

#include "sls.h"
#include "slsmm.h"
#include "sls_channel.h"
#include "sls_data.h"
#include "sls_ioctl.h"
#include "sls_path.h"

#include <slos.h>
#include "../slos/slos_record.h"
#include "../slos/slos_io.h"
#include "../slos/slos_inode.h"
#include "../slos/slos_inode.h"
	    
/* This should work both with deltas and without with the current code. */
static int
sls_dump_pages_slos(vm_offset_t vaddr, size_t len, vm_offset_t data, struct slos_node *vp)
{
	uint64_t rno;
	int error;
	struct uio auio;
	struct iovec aiov;

	/* Check whether we already have a data record. */
	error = slos_firstrno_typed(vp, SLOSREC_DATA, &rno);
	if (error == EINVAL) {
	    /* Create the SLOS record for the metadata. */
	    error = slos_rcreate(vp, SLOSREC_DATA, &rno);
	    if (error != 0)
		return error;
	} else if (error != 0) {
	    /* True error, abort. */

	    return error;
	}

	/* Create the UIO for the disk. */
	aiov.iov_base = (void *) data;
	aiov.iov_len = len;
	slos_uioinit(&auio, vaddr, UIO_WRITE, &aiov, 1);

	/* The write itself. */
	error = slos_rwrite(vp, rno, &auio);
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_dump_pages_file(vm_offset_t vaddr, size_t size, vm_offset_t data, struct file *fp)
{
	int error; 

	error = sls_file_write(&vaddr, sizeof(vaddr), fp);
	if (error != 0)
	    return error;

	error = sls_file_write(&size, sizeof(size), fp);
	if (error != 0)
	    return error;
	
	error = sls_file_write((void*) data, size, fp);
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_dump_pages_chan(vm_offset_t vaddr, size_t size, vm_offset_t data, struct sls_channel *chan)
{
	int error;
    
	switch (chan->type) {
	case SLS_FILE:
	    error = sls_dump_pages_file(vaddr, size, data, chan->fp);
	    break;

	case SLS_OSD:
	    error = sls_dump_pages_slos(vaddr, size, data, chan->vp);
	    break;

	default:
	    return EINVAL;
	}

	return error;
}

static size_t
sls_contig_pages(vm_object_t obj, vm_page_t *page)
{
	vm_page_t prev, cur;
	size_t contig_size; 

	prev = *page;
	cur = TAILQ_NEXT(prev, listq);
	contig_size = pagesizes[prev->psind];

	TAILQ_FOREACH_FROM(cur, &obj->memq, listq) {
	    /* Pages need to be physically and logically contiguous. */
	    if (prev->phys_addr + pagesizes[prev->psind] != cur->phys_addr ||
		prev->pindex + OFF_TO_IDX(pagesizes[prev->psind]) != cur->pindex)
		break;
	
	    if (contig_size > sls_contig_limit)
		break;

	    contig_size += pagesizes[cur->psind];
	    prev = cur;
	}

	*page = cur;

	return contig_size;
}

static int
sls_dump_pages(struct vmspace *vm, struct sls_channel *chan, int mode)
{
	vm_offset_t sentinel = ULONG_MAX;
	vm_offset_t vaddr, data;
	struct vm_map_entry *entry;
	struct vm_map *map;
	vm_offset_t offset;
	vm_object_t obj;
	vm_page_t startpage, page;
	size_t contig_size;
	int error;
	
	map = &vm->vm_map;
	for (entry = map->header.next; entry != &map->header; entry = entry->next) {

	    offset = entry->offset;
	    obj = entry->object.vm_object;

	    for (;;) {
		if (obj == NULL || obj->type != OBJT_DEFAULT)
		    break;

		page = TAILQ_FIRST(&obj->memq); 
		while (page != NULL) {
		    startpage = page;
		    vaddr = IDX_TO_VADDR(startpage->pindex, entry->start, offset);
		    contig_size = sls_contig_pages(obj, &page);

		    data = pmap_map(NULL, startpage->phys_addr, 
			    startpage->phys_addr + contig_size, 
			    VM_PROT_READ | VM_PROT_WRITE);
		    if (data == 0)
			return ENOMEM;

		    error = sls_dump_pages_chan(vaddr, contig_size, data, chan);
		    if (error != 0)
			return error;

		    /* XXX Do we need pmap_delete or something of the sort? */

		    /* We have looped around */
		    if (page == NULL || 
			startpage->pindex >= page->pindex)
			break;
		}

		if (mode == SLS_DELTA)
		    break;

		offset += obj->backing_object_offset;
		obj = obj->backing_object;
	    }
	}

	/* Only need to write a sentinel address for file backends. */
	if (chan->type == SLS_FILE)
	    sls_file_write(&sentinel, sizeof(sentinel), chan->fp);

	if (error != 0)
	    return error;

	return 0;
}

int
sls_dump(struct sls_process *slsp, struct sls_channel *chan)
{
	int error = 0;
	struct vmspace *vm;
	char *buf;
	size_t len;

	vm = slsp->slsp_vm;

	/* First write the coalesced metadata */
	/* 
	 * XXX Unpack the different records to be able to
	 * store semantic information alongside them.
	 * Not useful for the file, but necessary for
	 * the OSD. Right now we write everything in
	 * a single SLOS process file.
	 */
	error = sbuf_finish(slsp->slsp_ckptbuf);
	if (error != 0)
	    return error;

	buf = sbuf_data(slsp->slsp_ckptbuf);
	len = sbuf_len(slsp->slsp_ckptbuf);

	/* Write the data to the channel. */
	error = sls_write(buf, len, SLOSREC_PROC, chan);

	sbuf_clear(slsp->slsp_ckptbuf);

	if(error != 0)
	    return error;

	error = sls_dump_pages(vm, chan, slsp->slsp_attr.attr_mode);
	if (error != 0) {
	    printf("Error: Dumping pages failed with %d\n", error);
	    return error;
	}

	return 0;
}

