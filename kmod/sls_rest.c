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

static int
sls_rest_cpustate(struct proc *p, struct file *fp)
{
	int error = 0;
	struct proc_info proc_info; 
	struct thread_info *thread_infos;

	error = sls_load_cpustate(&proc_info, &thread_infos, fp);
	if (error != 0)
	    return error;

	error = proc_rest(p, &proc_info, thread_infos);
	free(thread_infos, M_SLSMM);

	return error;
}

static int
sls_rest_filedesc(struct proc *p, struct file *fp)
{
	int error = 0;
	struct filedesc_info filedesc_info;

	error = sls_load_filedesc(&filedesc_info, fp);
	if (error != 0)
	    return error;

	error = fd_rest(p, filedesc_info);

	/* 
	 * XXX free filedesc-related resources.
	 * Again, read the data at a higher 
	 * granularity to make this easier.
	 */

	return error;
}

static int
sls_rest_memory(struct proc *p, struct file *fp)
{
	int error;
	struct memckpt_info memory;
	struct sls_pagetable ptable;

	/* 
	 * XXX We don't bother with cleanup right now,
	 * we need to read and restore at a higher 
	 * granularity. Next patch will fix that.
	 */
	error = sls_load_memory(&memory, fp);
	if (error != 0)
	    goto sls_rest_memory_out;

	error = sls_load_ptable(&ptable, fp);
	if (error != 0)
	    goto sls_rest_memory_out;

	error = vmspace_rest(p, memory, ptable);
	if (error != 0) {
	    printf("Error: vmspace_restore failed with error code %d\n", error);
	    goto sls_rest_memory_out;
	}

sls_rest_memory_out:


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

	SLS_DBG("cpu restored\n");
	error = sls_rest_filedesc(p, fp);
	if (error != 0)
	    goto sls_rest_out;

	SLS_DBG("fd restored\n");
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
    kthread_exit();
}
