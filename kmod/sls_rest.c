#include <sys/types.h>

#include <sys/capsicum.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/queue.h>
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

#include "sls.h"
#include "slsmm.h"
#include "sls_ioctl.h"
#include "sls_data.h"

#include "sls_op.h"
#include "sls_dump.h"
#include "sls_objtable.h"

static int
sls_rest_thread(struct proc *p, struct file *fp)
{
	struct thread_info thread_info;
	int error;

	error = sls_load_thread(&thread_info, fp);
	if (error != 0)
	    return error;

	error = sls_thread_rest(p, &thread_info);
	if (error != 0)
	    return error;

	return 0;
}

static int
sls_rest_cpustate(struct proc *p, struct file *fp)
{
	struct proc_info proc_info; 
	int error, i;

	error = sls_load_proc(&proc_info, fp);
	if (error != 0)
	    return error;

	error = sls_proc_rest(p, &proc_info);
	if (error != 0)
	    return error;

	for (i = 0; i < proc_info.nthreads; i++) {
	    error = sls_rest_thread(p, fp);
	    if (error != 0)
		return error;
	}

	return 0;
}

static int
sls_rest_file(struct proc *p, int *done, struct file *fp)
{
	struct file_info file_info;
	int error;

	*done = 0;

	error = sls_load_file(&file_info, fp);
	if (error != 0)
	    return error;

	if (file_info.magic == SLS_FILES_END) {
	    *done = 1;
	    return 0;
	}

	error = sls_file_rest(p, &file_info);
	sbuf_delete(file_info.path);

	if (error != 0)
	    return error;

	return 0;
}

static int
sls_rest_filedesc(struct proc *p, struct file *fp)
{
	int error = 0;
	struct filedesc_info filedesc_info;
	int done;

	error = sls_load_filedesc(&filedesc_info, fp);
	if (error != 0)
	    return error;

	error = sls_filedesc_rest(p, filedesc_info);
	sbuf_delete(filedesc_info.cdir);
	sbuf_delete(filedesc_info.rdir);
	if (error != 0)
	    return error;

	do {
	    error = sls_rest_file(p, &done, fp);
	    if (error != 0)
		return error;
	} while (!done);


	return error;
}

static int
sls_rest_memory(struct proc *p, struct file *fp)
{
	struct memckpt_info memory;
	struct vm_object_info obj_info;
	struct vm_map_entry_info entry_info;
	struct sls_pagetable ptable;
	struct sls_objtable objtable;
	vm_map_t map;
	vm_map_entry_t entry;
	int error, i;

	error = sls_objtable_init(&objtable);
	if (error != 0)
	    return error;

	error = sls_load_memory(&memory, fp);
	if (error != 0)
	    goto sls_rest_memory_out;

	error = sls_vmspace_rest(p, memory);
	if (error != 0)
	    goto sls_rest_memory_out;

	map = &p->p_vmspace->vm_map;

	/* Set up the object table */
	for (;;) {
	    error = sls_load_vmobject(&obj_info, fp);
	    if (error != 0)
		goto sls_rest_memory_out;

	    if (obj_info.magic == SLS_OBJECTS_END)
		break;

	    error = sls_vmobject_rest(&obj_info, &objtable);
	    if (error != 0)
		goto sls_rest_memory_out;
	}

	for (i = 0; i < memory.vmspace.nentries; i++) {
	    error = sls_load_vmentry(&entry_info, fp);
	    if (error != 0)
		goto sls_rest_memory_out;

	    PROC_UNLOCK(p);
	    error = sls_vmentry_rest(map, &entry_info, &objtable);
	    PROC_LOCK(p);
	    if (error != 0)
		goto sls_rest_memory_out;
	}
	

	error = sls_load_ptable(&ptable, fp);
	if (error != 0)
	    goto sls_rest_memory_out;


	/* Temporary measure until we start associating pages w/ objects */
	for (entry = map->header.next; entry !=  &map->header; entry = entry->next)
	    sls_data_rest(ptable, map, entry);

sls_rest_memory_out:

	sls_objtable_fini(&objtable);

	return error;
}

static int
sls_rest(struct proc *p, struct file *fp)
{
	int error;

	/*
	* XXX We don't actually need that, right? We're overwriting ourselves,
	* so we definitely don't want to stop.
	*/
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	/* 
	 * The order matters of these matters.
	 * We are reading data from a file with each call.
	 */
	error = sls_rest_cpustate(p, fp);
	if (error != 0)
	    goto sls_rest_out;

	error = sls_rest_filedesc(p, fp);
	if (error != 0)
	    goto sls_rest_out;

	error = sls_rest_memory(p, fp);
	if (error != 0)
	    goto sls_rest_out;

	SLS_DBG("memory restored\n");
	kern_psignal(p, SIGCONT);

sls_rest_out:
	PROC_UNLOCK(p);

	SLS_DBG("error %d\n", error);

	return error;
}

/*
 * Since we're going to close all open files 
 * midway through the restore, we cannot keep
 * using the fd. We instead get a handle of the
 * struct file, which we free at the end of the 
 * restore.
 */
static int
sls_restd_open(char *filename, struct file **fpp)
{
    struct file *fp;
    int error;
    int fd;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR, S_IRWXU);
    if (error != 0) {
	printf("Error: Opening file failed with %d\n", error);
	return error;
    }
    fd = curthread->td_retval[0];

    error = fget_read(curthread, fd, &cap_read_rights, &fp);
    if (error != 0) {
	kern_close(curthread, fd);
	return error;
    }

    *fpp = fp;

    return 0;
}

void
sls_restd(struct sls_op_args *args)
{
    int error;
    struct file *fp = NULL;
    
    error = sls_restd_open(args->filename, &fp);
    if (error != 0)
	goto sls_restd_out;

    error = sls_rest(args->p, fp);
    if (error != 0)
	printf("Error: sls_rest failed with %d\n", error);

sls_restd_out:

    if (fp != NULL)
	fdrop(fp, curthread);

    PRELE(args->p);

    free(args->filename, M_SLSMM);
    free(args, M_SLSMM);

    dev_relthread(slsm.slsm_cdev, 1);

    if (error != 0)
	exit1(curthread, error, 0);
    else
	kthread_exit();
}
