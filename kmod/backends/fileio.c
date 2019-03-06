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
#include "worker.h"

/*
 * XXX We need to take a hard look at what a descriptor actually is,
 * rn this thing has a lot of baggage
 */
struct sls_desc
create_desc(void *index, int type)
{
    int error;
    int fd;
    //int md;

    switch (type) {
    case SLSMM_FD_FILE:
	printf("Name: %s\n", (char *) index);
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
	    .type = DESC_FD,
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
        return (struct sls_desc) {
            .type = DESC_OSD,
            .index = nvdimm,
        };

    default:
	printf("Invalid descriptor arguments\n");
	return (struct sls_desc) {
	    .type = DESCRIPTOR_SIZE,
	    .index = NULL,
	};
    }
}

void
destroy_desc(struct sls_desc desc)
{
    switch(desc.type) {
    case DESC_FD:
	kern_close(curthread, desc.desc);
	return;

    case DESC_OSD:
	return;

    default:
	return;
    }

}

int
fd_read(void* addr, size_t len, struct sls_desc desc)
{
    switch (desc.type) {
    case DESC_FD:
	return file_read(addr, len, desc.desc);

    /*
    case DESC_MEM:
	return mem_read(addr, len, index);
    */

    case DESC_OSD:
	return nvdimm_read(addr, len, (vm_offset_t) desc.index);

    default:
	printf("fd_read: invalid desc with type %d\n", desc.type);
	return EINVAL;
    }
}

int
fd_write(void* addr, size_t len, struct sls_desc desc)
{
    switch (desc.type) {
    case DESC_FD:
	return file_write(addr, len, desc.desc);

    /*
    case DESC_MD:
	return mem_write(addr, len, index);
    */

    case DESC_OSD:
	return nvdimm_write(addr, len, (vm_offset_t) desc.index);

    default:
	printf("fd_write: invalid desc with type %d\n", desc.type);
	return EINVAL;
    }
}

void 
fd_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
	size_t numentries, struct sls_desc desc)
{
    switch (desc.type) {
    case DESC_FD:
	return file_dump(entries, objects, numentries, desc.desc);

    /*
    case DESC_MD:
	return mem_write(addr, len, index);
    */

    case DESC_OSD:
	return nvdimm_dump(entries, objects, numentries, desc.index);

    default:
	printf("fd_write: invalid desc with type %d\n", desc.type);
	return;
    }
}

#define NAME_SIZE (64)
/* 
 * XXX Make it so that calls for a specific backend succeed iff the backend was
 * initialized successfully. 
 */
void
backends_init(void)
{
	int error;

	error = sls_workers_init();
	if (error != 0) {
	    printf("sls_workers_init failed with %d\n", error);
	    return;
	}

	//md_init();

	if (nvdimm_open() != 0)
	    printf("Warning: nvdimm not opened due to error %d\n", error);

}

void
backends_fini(void)
{
	struct sls_worker *worker;
	int i;

	nvdimm_close();

	//md_clear();
	
	for (i = 0; i < WORKER_THREADS; i++) {
	    worker = &sls_workers[i];

	    mtx_destroy(&worker->work_mtx);
	    cv_destroy(&worker->work_cv);
	}

}
