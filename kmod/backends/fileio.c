#include <sys/param.h>

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/capsicum.h>
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
#include <sys/stat.h>
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
#include "nvdimm.h"


/*
 * XXX We need to take a hard look at what a descriptor actually is,
 * rn this thing has a lot of baggage
 */
struct sls_desc
create_desc(long index, int type)
{
    int error;
    int fd;
    //int md;

    switch (type) {
    case SLSMM_FD_FILE:
	error = kern_openat(curthread, AT_FDCWD, (char *) index, UIO_SYSSPACE,
	    O_RDWR | O_CREAT, S_IRWXU);
	if (error != 0) {
	    printf("kern_openat failed with %d\n", error);
	    return (struct sls_desc) {
		.type = DESCRIPTOR_SIZE,
		.desc = 0, 
	    };
	}

	fd = curthread->td_retval[0];
	return (struct sls_desc) {
	    .type = DESC_FILE,
	    .desc = fd, 
	};

    /* XXX temp fixup, we'll get it back */
    /*
    case SLSMM_FD_MEM:
	md = restoring ? index : new_md();
        return (struct sls_desc) {
            .type = DESC_MD,
            .index = md,
        };
    */

    case SLSMM_FD_NVDIMM:
	/* 
	 * Right now we treat the NVDIMM as a simple buffer, and
	 * it always starts at the beginning of the nvdimm area.
	 */
        return (struct sls_desc) {
            .type = DESC_OSD,
            .index = (uintptr_t) nvdimm,
        };

    /* XXX Problematic, we try to close it with desc_destroy() rn */
    case SLSMM_FD_FD:
        return (struct sls_desc) {
            .type = DESC_FD,
            .desc= index,
        };

    default:
	printf("Invalid descriptor arguments\n");
	return (struct sls_desc) {
	    .type = DESCRIPTOR_SIZE,
	    .index = 0,
	};
    }
}

void
destroy_desc(struct sls_desc desc)
{
    switch(desc.type) {
    case DESC_FILE:
	kern_close(curthread, desc.desc);
	return;

    case DESC_OSD:
	return;

    case DESC_FD:
	return;

    default:
	return;
    }

}

int
fd_read(void* addr, size_t len, struct sls_desc *desc)
{
    switch (desc->type) {
    case DESC_FILE:
    case DESC_FD:
	return file_read(addr, len, desc->desc);

    /*
    case DESC_MEM:
	return mem_read(addr, len, index);
    */

    case DESC_OSD:
	nvdimm_read(addr, len, &desc->index);
	desc->index += len;
	return 0;

    default:
	printf("fd_read: invalid desc with type %d\n", desc->type);
	return EINVAL;
    }
}

int
fd_write(void* addr, size_t len, struct sls_desc *desc)
{
    switch (desc->type) {
    case DESC_FILE:
    case DESC_FD:
	return file_write(addr, len, desc->desc);

    /*
    case DESC_MD:
	return mem_write(addr, len, index);
    */

    case DESC_OSD:
	nvdimm_write(addr, len, &desc->index);
	desc->index += len;
	return 0;

    default:
	printf("fd_write: invalid desc with type %d\n", desc->type);
	return EINVAL;
    }
}

void 
fd_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
	size_t numentries, struct sls_desc *desc, int mode)
{
    switch (desc->type) {
    case DESC_FILE:
    case DESC_FD:
	file_dump(entries, objects, numentries, desc->desc, mode);
	return;

    /*
    case DESC_MD:
	return mem_write(addr, len, index);
    */

    case DESC_OSD:
	nvdimm_dump(entries, objects, numentries, &desc->index, mode);
	return;

    default:
	printf("fd_write: invalid desc with type %d\n", desc->type);
	return;
    }
}


/* 
 * XXX Make it so that calls for a specific backend succeed iff the backend was
 * initialized successfully. 
 */
void
backends_init(void)
{
	int error;

	//md_init();
	
	error = nvdimm_open();
	if (error != 0) {
	    printf("Warning: nvdimm not opened due to error %d. ", error);
	    printf("Defaulting to using RAM as NVDIMM\n");

	    nvdimm = malloc(1024L * 1024 * 1024, M_SLSMM, M_WAITOK);
	    if (nvdimm == NULL) {
		printf("Allocation failed, DO NOT use the OSD\n");
	    } else {
		printf("'NVDIMM' space is %ld bytes large\n", 1024L * 1024 * 1024);
		nvdimm_size = 1024L * 1024 * 1024;
	    }
	}

	error = sls_workers_init();
	if (error != 0) {
	    printf("sls_workers_init failed with %d\n", error);
	    return;
	}
}

void
backends_fini(void)
{
	struct sls_worker *worker;
	int i;


	//md_clear();
	
	for (i = 0; i < WORKER_THREADS; i++) {
	    worker = &sls_workers[i];

	    mtx_destroy(&worker->work_mtx);
	    cv_destroy(&worker->work_cv);
	}

	nvdimm_close();

}

