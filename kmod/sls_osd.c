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
#include "sls_file.h"
#include "sls_snapshot.h"


struct slsosd osd;
int osdfd;

int
osd_import(void)
{
    int error;

    error = file_pread(&osd, sizeof(osd), osdfd, 0);
    if (error != 0)
	return error;

    if (osd.osd_magic != SLSOSD_MAGIC)
	return EINVAL;

    if (osd.osd_majver > SLSOSD_MAJOR_VERSION)
	return EINVAL;

    /* XXX How do we handle minor versions? */


    /* XXX fsck-like utility if osd_clean is unset */
}


struct slsosd_inode
osd_get_inode(void)
{
    char *block;

    block = malloc(osd_bsize, M_SLSMM, M_WAITOK);

    error = file_pread(block, osd.osd_bsize, osdfd, osd.osd_inodeoff);
    if (error != 0)
	return error;
    

}

struct slsosd_ptr
osd_get_block(void)
{

}



int
export_pages()
{
    /* Iterate the dump's pages, store each one */
}

int
export_dump(struct sls_snapshot *slss, int fd)
{
	int i;
	int error = 0;
	struct vm_map_entry_info *entries;
	struct thread_info *thread_infos;
	struct file_info *file_infos;
	size_t cdir_len, rdir_len;
	struct vm_object_info *cur_obj;
	int numthreads, numentries, numfiles;
	struct dump *dump;

	dump = slss->slss_dump;
	thread_infos = dump->threads;
	entries = dump->memory.entries;
	file_infos = dump->filedesc.infos;

	numthreads = dump->proc.nthreads;
	numentries = dump->memory.vmspace.nentries;
	numfiles = dump->filedesc.num_files;

	for (i = 0; i < numentries; i++) {
	    if (entries->eflags & MAP_ENTRY_IS_SUB_MAP) {
		printf("WARNING: Submap entry found, dump will be wrong\n");
		continue;
	    }
	}

	error = file_write(dump, sizeof(struct dump), fd);
	if (error != 0) {
	    printf("Error: Writing dump failed with code %d\n", error);
	    return error;
	}

	error = file_write(thread_infos, sizeof(struct thread_info) * numthreads, fd);
	if (error != 0) {
	    printf("Error: Writing thread info dump failed with code %d\n", error);
	    return error;
	}

	error = file_write(file_infos, sizeof(struct file_info) * numfiles, fd);
	if (error != 0) {
	    printf("Error: Writing file info dump failed with code %d\n", error);
	    return error;
	}

	error = file_write(entries, sizeof(*entries) * numentries, fd);
	if (error != 0) {
	    printf("Error: Writing entry info dump failed with code %d\n", error);
	    return error;
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;
	    if (cur_obj == NULL)
		continue;

	    error = file_write(cur_obj, sizeof(*cur_obj), fd);
	    if (error != 0) {
		printf("Error: Writing object info failed with code %d\n", error);
		return error;
	    }

	}

	cdir_len = dump->filedesc.cdir_len;
	error = file_write(dump->filedesc.cdir, cdir_len, fd);
	if (error != 0) {
	    printf("Error: Writing cdir path failed with code %d\n", error);
	    return error;
	}

	rdir_len = dump->filedesc.rdir_len;
	error = file_write(dump->filedesc.rdir, rdir_len, fd);
	if (error != 0) {
	    printf("Error: Writing cdir path failed with code %d\n", error);
	    return error;
	}


	for (i = 0; i < numfiles; i++) {
	    error = file_write(file_infos[i].filename, file_infos[i].filename_len, fd);
	    if (error != 0) {
		printf("Error: Writing filename failed with code %d\n", error);
		return error;
	    }
	}

	for (i = 0; i < numentries; i++) {
	    cur_obj = entries[i].obj_info;

	    if (cur_obj != NULL && cur_obj->filename != NULL) {
		error = file_write(cur_obj->filename, cur_obj->filename_len, fd);
		if (error != 0) {
		    printf("Error: Could not write filename\n");
		    return error;
		}
	    }
	}
	

	error = export_pages(vm, mode);
	if (error != 0) {
	    printf("Error: Dumping pages failed with %d\n", error);
	    return error;
	}

	return 0;
}
