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
#include "sls_data.h"
#include "sls_dump.h"
#include "sls_ioctl.h"
#include "sls_file.h"
#include "sls_path.h"

static int
sls_dump_pages_file(vm_offset_t vaddr, size_t size, vm_offset_t data, int fd)
{
	int error; 

	error = sls_fd_write(&vaddr, sizeof(vaddr), fd);
	if (error != 0)
	    return error;

	error = sls_fd_write(&size, sizeof(size), fd);
	if (error != 0)
	    return error;
	
	error = sls_fd_write((void*) data, size, fd);
	if (error != 0)
	    return error;

	return 0;
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
sls_dump_pages(struct vmspace *vm, struct sls_store_tgt tgt, int mode)
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

		    error = sls_dump_pages_file(vaddr, contig_size, data, tgt.fd);
		    /* XXX Do we need pmap_delete or something of the sort? */

		    if (error != 0)
			return error;

		    /* We have looped around */
		    if (page == NULL || 
			startpage->pindex >= page->pindex)
			break;
		}

		if (mode == SLS_CKPT_DELTA)
		    break;

		offset += obj->backing_object_offset;
		obj = obj->backing_object;
	    }
	}

	if (tgt.type == SLS_MEM)
	    return 0;

	sls_fd_write(&sentinel, sizeof(sentinel), tgt.fd);
	if (error != 0)
	    return error;

	return 0;

}

int
sls_dump(struct sls_process *slsp, int mode, int fd)
{
	int error = 0;
	struct sls_store_tgt tgt;
	struct vmspace *vm;
	char *buf;
	size_t len;

	vm = slsp->slsp_vm;

	/* First write the coalesced metadata */
	/* 
	 * XXX Unpack them maybe to be able to
	 * store semantic information alongside them.
	 * Not useful for the file, but necessary for
	 * the OSD.
	 */
	error = sbuf_finish(slsp->slsp_ckptbuf);
	if (error != 0)
	    return error;

	buf = sbuf_data(slsp->slsp_ckptbuf);
	len = sbuf_len(slsp->slsp_ckptbuf);

	error = sls_fd_write(buf, len, fd);

	sbuf_clear(slsp->slsp_ckptbuf);

	if(error != 0)
	    return error;

	tgt = (struct sls_store_tgt) {
	    .type = SLS_FILE,
	    .fd	  = fd,
	};

	error = sls_dump_pages(vm, tgt, mode);
	if (error != 0) {
	    printf("Error: Dumping pages failed with %d\n", error);
	    return error;
	}

	return 0;
}

