#include <sys/types.h>

#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
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
#include <sys/stat.h>
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
#include "sls_file.h"
#include "sls_dump.h"
#include "sls_ioctl.h"
#include "sls_file.h"
#include "sls_osd.h"

/* Oof */
int osdfd;
static int times_called = 0;
/* Hack*/
/* Fuck it, start right after the super*/
uint64_t curblk = 1;

/* 
 * TODO: Connect the pages full of vaddr to the mino 
 * (this is where we weld the abstractions we discussed).
 * Right now we aren't saving the virtual addresses, or 
 * connecting the written data to the inode.
 */

static void
osd_fillmino(struct osd_mino *mino, void *data, size_t len, uint32_t type)
{
    struct osd_mrec *mrec;
	
    mrec = mrec_alloc(slsm.slsm_mbmp, 0 , 0);
    mrec->mrec_type = type;
    mrec->mrec_bufinuse = 1;
    mrec_addbuf(mrec, data, len);

    mino_addmrec(mino, mrec);
}

static void
store_pages_osd(struct osd_mbmp *mbmp, struct iovec *iov, size_t iovlen, void *vaddrs)
{
    int i;
    uint64_t block;
    uint64_t blocks_left, blkrange;
    struct iovec *newiov;
    uint64_t total_size, capacity_left;
    size_t newiovlen;
    uint64_t bsize;

    newiov = malloc(sizeof(*newiov) * UIO_MAXIOV, M_SLSMM, M_WAITOK);
    bsize = mbmp->mbmp_osd->osd_bsize;

    total_size = 0;
    for (i = 0; i < iovlen; i++)
	total_size += iov[i].iov_len;

    blocks_left = total_size / bsize;
    if (total_size % bsize != 0)
	blocks_left += 1;

    i = 0;
    newiovlen = 0;
    while (blocks_left > 0) {
	if (i >= iovlen)
	    break;
	/*
	blkrange = blocks_left;
	block = blk_getrange(mbmp, &blkrange); 
	capacity_left = blkrange * bsize;
	blocks_left -= blkrange;
	*/

	/* 8MB transfers */
	blkrange = 4096;

	/* Wrap around */
	if (curblk > 100 * 2 * 1024 * 1024)
	    curblk = 1024;

	block = curblk;
	curblk += blkrange;
	capacity_left = blkrange * bsize;
	blocks_left -= blkrange;

	while (capacity_left > 0 && i < iovlen) {
	    if (capacity_left >= iov[i].iov_len) {
		newiov[newiovlen].iov_base = iov[i].iov_base;
		newiov[newiovlen].iov_len = iov[i].iov_len;
		i += 1;
		capacity_left -= iov[i].iov_len;
	    } else {
		newiov[newiovlen].iov_base = iov[i].iov_base;
		newiov[newiovlen].iov_len = capacity_left;

		iov[i].iov_base = (void *) ((uint64_t) iov[i].iov_base + capacity_left);
		iov[i].iov_len -= capacity_left;

		capacity_left = 0;
	    }

	    newiovlen += 1;

	    if (newiovlen == 64) {
		times_called += 1;
		osd_pwritev(mbmp, block, newiov, newiovlen);
		newiovlen = 0;
	    }
	}

	if (newiovlen != 0) {
	    osd_pwritev(mbmp, block, newiov, newiovlen);
	    times_called++;
	    newiovlen = 0;
	}

    }

    
    free(newiov, M_SLSMM);
}

/*
 * This is similar enough (read: almost identical) to the
 * code in sls_dump.c. That means that if store_pages_osd above 
 * gets moved there we should be fine.
 */
static int
osd_store(struct osd_mino *mino, struct vmspace *vm, int mode)
{
	vm_offset_t vaddr, vaddr_data;
	struct vm_map_entry *entry;
	struct vm_map *map;
	vm_offset_t offset;
	vm_object_t obj;
	vm_page_t page;
	vm_paddr_t pagerun_start_phys;
	struct iovec *iov;
	vm_offset_t *vaddrs; 
	size_t iovlen;
	vm_offset_t pagerun_start, pagerun_len;
	size_t curpage_size;
	int i;

	iov = malloc(sizeof(*iov) * UIO_MAXIOV, M_SLSMM, M_WAITOK);
	vaddrs = malloc(sizeof(*vaddrs) * UIO_MAXIOV, M_SLSMM, M_WAITOK);
	iovlen = 0;
	curpage_size = 0;

	/*
	 * We group pages into page runs; sequences of pages that are contiguous
	 * in physical memory. Since we need to save their position in the user's
	 * address space, they also need to be contiguous there, too.
	 */
	pagerun_start = 0;
	pagerun_start_phys = 0;
	pagerun_len = 0;
	
	map = &vm->vm_map;
	for (entry = map->header.next; entry != &map->header; entry = entry->next) {

	    offset = entry->offset;
	    obj = entry->object.vm_object;

	    while (obj != NULL) {
		if (obj->type != OBJT_DEFAULT)
		    break;

		TAILQ_FOREACH(page, &obj->memq, listq) {
	    
		    KASSERT(page != NULL, "page is NULL\n");
		    
		    vaddr = IDX_TO_VADDR(page->pindex, entry->start, offset);
		    curpage_size = 1 << (page->order - 1);
		    
		    /* 
		     * Make sure that the new page is consecutive with the
		     * current page run both in physical memory and in the
		     * user address space.
		     */
		    if (vaddr == pagerun_start + pagerun_len && 
			page->phys_addr == pagerun_start_phys + pagerun_len &&
			pagerun_len < 2 * 1024 * 1024) {

			pagerun_len += curpage_size;
			continue;
		    }

		    /* 
		     * If it's 0, we don't have a previously 
		     * saved page run; initialize one here.
		     */
		    if (pagerun_start == 0) {
			pagerun_start = vaddr;
			pagerun_start_phys = page->phys_addr;
			pagerun_len = curpage_size;
			continue;
		    }

		    
		    /* Never fails on amd64, check is here for futureproofing */
		    vaddr_data = userpage_map(pagerun_start_phys, pagerun_len);
		    if ((void *) vaddr_data == NULL) {
			printf("Mapping page failed\n");
			free(iov, M_SLSMM);
			free(vaddrs, M_SLSMM);
			return ENOMEM;
		    }

		    /* XXX Do something about the addresses */
		    vaddrs[iovlen] = vaddr;
		    iov[iovlen].iov_base = (void *) vaddr_data;
		    iov[iovlen].iov_len = pagerun_len;
		    iovlen += 1;

		    /* 
		     * We only write the pages once we have a good amount 
		     * of them in the IO vector.
		     */
		    if (iovlen == 128) {
			/*
			store_pages_osd(mino->mino_mbmp, iov, iovlen, vaddrs);
			*/
			uint64_t b = curblk;
			for (int i = 0; i < iovlen; i++)
				curblk += (iov[i].iov_len / mino->mino_mbmp->mbmp_osd->osd_bsize);
			osd_pwritev(NULL, b, iov, iovlen);

			for (i = 0; i < iovlen; i++)
			    userpage_unmap((vm_offset_t) iov[i].iov_base);

			iovlen = 0;
		    }
		    
		    /* 
		     * We flushed the last page run, so set up a new one
		     * starting with the current page.
		     */
		    pagerun_start = vaddr;
		    pagerun_start_phys = page->phys_addr;
		    pagerun_len = curpage_size;
		}

		if (mode == SLS_CKPT_DELTA)
		    break;

		offset += obj->backing_object_offset;
		obj = obj->backing_object;
	    }
	}

	vaddr_data = userpage_map(pagerun_start_phys, pagerun_len);
	if ((void *) vaddr_data == NULL) {
	    printf("Mapping page failed\n");
	    free(iov, M_SLSMM);
	    free(vaddrs, M_SLSMM);
	    return ENOMEM;
	}

	//vaddrs[iovlen] = pagerun_start;
	iov[iovlen].iov_base = (void *) vaddr_data;
	iov[iovlen].iov_len = pagerun_len;
	iovlen += 1;

	/* Store any leftover pages */
	store_pages_osd(mino->mino_mbmp, iov, iovlen, vaddrs);

	for (i = 0; i < iovlen; i++)
	    userpage_unmap((vm_offset_t) iov[i].iov_base);


	free(iov, M_SLSMM);
	free(vaddrs, M_SLSMM);
	
	return 0;

}

int
osd_dump(struct dump *dump, struct vmspace *vm, int mode)
{
	int i;
	int error = 0;
	struct vm_map_entry_info *entries;
	struct thread_info *thread_infos;
	struct file_info *file_infos;
	struct vm_object_info *cur_obj;
	int numthreads, numentries, numfiles;
	struct osd_mino *mino;
	char *path;
	size_t len;

	/* HACK */
	size_sent = 0;
	times_called = 0;

	thread_infos = dump->threads;
	entries = dump->memory.entries;
	file_infos = dump->filedesc.infos;

	numthreads = dump->proc.nthreads;
	numentries = dump->memory.vmspace.nentries;
	numfiles = dump->filedesc.num_files;

	mino = mino_alloc(slsm.slsm_mbmp, 0);
	if (mino == NULL) {
	    printf("Error: mino is NULL\n");
	    return ENOMEM;
	}

	for (i = 0; i < numentries; i++) {
	    if (entries->eflags & MAP_ENTRY_IS_SUB_MAP) {
		printf("WARNING: Submap entry found, dump will be wrong\n");
		continue;
	    }
	}


	osd_fillmino(mino, &dump->proc, sizeof(dump->proc), SLSREC_PROC);
	osd_fillmino(mino, thread_infos, sizeof(*thread_infos) * numthreads, SLSREC_TD);
	osd_fillmino(mino, &dump->filedesc, sizeof(dump->filedesc), SLSREC_FDESC);
	osd_fillmino(mino, file_infos, sizeof(*file_infos) * numfiles, SLSREC_FILE);
	osd_fillmino(mino, &dump->memory, sizeof(dump->memory), SLSREC_MEM);
	osd_fillmino(mino, entries, sizeof(*entries) * numentries, SLSREC_MEMENTRY);

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;
	    if (cur_obj == NULL)
		continue;

	    osd_fillmino(mino, cur_obj, sizeof(*cur_obj), SLSREC_MEMOBJT);
	}

	path = sbuf_data(dump->filedesc.cdir);
	len = sbuf_len(dump->filedesc.cdir);
	osd_fillmino(mino, path, len, SLSREC_FILENAME);

	path = sbuf_data(dump->filedesc.rdir);
	len = sbuf_len(dump->filedesc.rdir);
	osd_fillmino(mino, path, len, SLSREC_FILENAME);


	for (i = 0; i < numfiles; i++) {
	    path = sbuf_data(file_infos[i].path);
	    len = sbuf_len(file_infos[i].path);
	    osd_fillmino(mino, path, len, SLSREC_FILENAME);
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;

	    if (cur_obj != NULL && cur_obj->path != NULL) {
		path = sbuf_data(cur_obj->path);
		len = sbuf_len(cur_obj->path);
		osd_fillmino(mino, path, len, SLSREC_FILENAME);
	    }
	}

	error = osd_store(mino, vm, mode);
	if (error != 0) {
	    printf("Error: Dumping pages failed with %d\n", error);
	    return error;
	}
	printf("Stored it all\n");

	/*
	error = mino_write(mino);
	if (error != 0) {
	    printf("Writing inode and metadata failed with %d\n", error);
	    return error;
	}
	*/

	printf("Total size sent in MB: %lu\n", size_sent / (1024 * 1024));
	printf("Total times called: %u\n", times_called);

	return 0;
}

