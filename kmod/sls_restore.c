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
#include "sls_process.h"
#include "sls_shadow.h"



static int
sls_restore(struct proc *p, struct sls_process *slsp)
{
	struct dump *dump;
	int error = 0;

	dump = slsp->slsp_dump;

	/*
	* XXX We don't actually need that, right? We're overwriting ourselves,
	* so we definitely don't want to stop.
	*/
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	error = proc_restore(p, &dump->proc, dump->threads);
	if (error != 0) {
	    printf("Error: reg_restore failed with error code %d\n", error);
	    goto sls_restore_out;
	}

	error = vmspace_restore(p, slsp, &dump->memory);
	if (error != 0) {
	    printf("Error: vmspace_restore failed with error code %d\n", error);
	    goto sls_restore_out;
	}

	error = fd_restore(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_restore failed with error code %d\n", error);
	    goto sls_restore_out;
	}

	kern_psignal(p, SIGCONT);

sls_restore_out:
	PROC_UNLOCK(p);

	return error;
}

static struct sls_process * 
slsp_from_file(char *filename)
{
    struct sls_process *slsp;
    int error;
    int fd;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR | O_CREAT, S_IRWXU);
    fd = curthread->td_retval[0];
    if (error != 0) {
	printf("Error: Opening file failed with %d\n", error);
	return NULL;
    }

    slsp = load_dump(fd);
    kern_close(curthread, fd);

    return slsp;
}


void
sls_restored(struct sls_op_args *args)
{
    struct sls_process *slsp = NULL;
    int error;

    if (args->fd_type == SLS_FD_FILE)
	slsp = slsp_from_file(args->filename);
    else
	slsp = slsp_find(args->id);
    
    printf("Arguments searched\n");
    if (slsp == NULL)
	goto sls_restored_out;
	
    error = sls_restore(args->p, slsp);
    if (error != 0)
	printf("Error: sls_restore failed with %d\n", error);
    printf("Restore done\n");

sls_restored_out:
    PRELE(args->p);

    slsp_fini(slsp);
    free(args->filename, M_SLSMM);
    free(args, M_SLSMM);

    dev_relthread(sls_metadata.slsm_cdev, 1);
    kthread_exit();
}
