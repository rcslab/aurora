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
#include "sls_snapshot.h"



static int
sls_rest(struct proc *p, struct sls_snapshot *slss)
{
	struct dump *dump;
	int error = 0;

	dump = slss->slss_dump;

	/*
	* XXX We don't actually need that, right? We're overwriting ourselves,
	* so we definitely don't want to stop.
	*/
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	error = proc_rest(p, &dump->proc, dump->threads);
	if (error != 0) {
	    printf("Error: reg_restore failed with error code %d\n", error);
	    goto sls_rest_out;
	}

	error = vmspace_rest(p, slss, &dump->memory);
	if (error != 0) {
	    printf("Error: vmspace_restore failed with error code %d\n", error);
	    goto sls_rest_out;
	}

	error = fd_rest(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_restore failed with error code %d\n", error);
	    goto sls_rest_out;
	}

	kern_psignal(p, SIGCONT);

sls_rest_out:
	PROC_UNLOCK(p);

	return error;
}

void
sls_restd(struct sls_op_args *args)
{
    int error;
	
    error = sls_rest(args->p, args->slss);
    if (error != 0)
	printf("Error: sls_rest failed with %d\n", error);

    PRELE(args->p);

    if (args->target == SLS_FILE)
	slss_fini(args->slss);
    free(args->filename, M_SLSMM);
    free(args, M_SLSMM);

    dev_relthread(slsm.slsm_cdev, 1);
    kthread_exit();
}
