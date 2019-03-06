#include <sys/param.h>

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>

#include "fileio.h"
#include "../memckpt.h"
#include "../_slsmm.h"
#include "../slsmm.h"
#include "file.h"


int
file_read(void* addr, size_t len, int fd)
{
	int error = 0;

	/* XXX Do the same modifications done in file_write */
	struct uio auio;
	struct iovec aiov;
	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_READ;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	error = kern_readv(curthread, fd, &auio);
	if (error) {
		printf("Error: kern_readv failed with code %d\n", error);
	}

	return error;
}

int
file_write(void* addr, size_t len, int fd)
{
	int error = 0;
	struct uio auio;
	struct iovec aiov;

	bzero(&auio, sizeof(struct uio));
	bzero(&aiov, sizeof(struct iovec));

	aiov.iov_base = (void*)addr;
	aiov.iov_len = len;

	auio.uio_iov = &aiov;
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	auio.uio_iovcnt = 1;
	auio.uio_resid = len;
	auio.uio_td = curthread;

	error = kern_writev(curthread, fd, &auio);
	if (error) {
		printf("Error: kern_readv failed with code %d\n", error);
	}
	return error;
}

void
file_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
	    	size_t numentries, int fd)
{
	vm_page_t page;
	vm_offset_t vaddr, vaddr_data;
	struct vm_map_entry_info *entry;
	int i;
	int error;
	int cnt = 0;

	for (i = 0; i < numentries; i++) {

	    if (objects[i] == NULL)
		continue;
	
	    entry = &entries[i];
	
	    TAILQ_FOREACH(page, &objects[i]->memq, listq) {
	
		/*
		* XXX Does this check make sense? We _are_ getting pages
		* from a valid object, after all, why would it have NULL
		* pointers in its list?
		*/
		if (!page) {
		    printf("ERROR: vm_page_t page is NULL");
		    continue;
		}
		
		vaddr_data = IDX_TO_VADDR(page->pindex, entry->start, entry->offset);
		error = file_write(&vaddr_data, sizeof(vm_offset_t), fd);
		if (error != 0) {
		    printf("Error: writing vm_map_entry_info failed\n");
		    return;
		    //return error;
		}
		
		/* Never fails on amd64, check is here for futureproofing */
		vaddr = userpage_map(page->phys_addr);
		if ((void *) vaddr == NULL) {
		    printf("Mapping page failed\n");
		    /* EINVAL seems the most appropriate */
		    error = EINVAL;
		    return;
		    //return error;
		}
		
		/* XXX parallelize */
		error = file_write((void*) vaddr, PAGE_SIZE, fd);
		if (error != 0)
		    return;
		    //return error;
		
		cnt++;
		userpage_unmap(vaddr);
	    }
	}
	printf("cnt = %d\n", cnt);
	
}
