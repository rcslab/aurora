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

static void
sls_checkpoint_dump_init(struct proc *p, struct dump *dump)
{
	struct thread_info *thread_infos = NULL;
	struct vm_map_entry_info *entries = NULL;
	struct file_info *file_infos = NULL;
	size_t numentries, numthreads, numfiles;

	numthreads = p->p_numthreads;
	numentries = p->p_vmspace->vm_map.nentries;
	numfiles = p->p_fd->fd_files->fdt_nfiles;

	thread_infos = malloc(sizeof(*thread_infos) * numthreads, M_SLSMM, M_WAITOK);
	entries = malloc(sizeof(*entries) * numentries, M_SLSMM, M_WAITOK);
	file_infos = malloc(sizeof(*file_infos) * numfiles, M_SLSMM, M_WAITOK);

	dump->threads = thread_infos;
	dump->memory.entries = entries;
	dump->filedesc.infos = file_infos;

}

static struct sls_process *
sls_checkpoint_slsp(struct proc *p, int mode)
{
    struct sls_process *slsp;
    slsp = slsp_init(p);
    sls_checkpoint_dump_init(p, slsp->slsp_dump);

    return slsp;
}

static void
sls_stop_proc(struct proc *p)
{
	int threads_still_running;
	struct thread *td;

	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);
	PROC_UNLOCK(p);

	threads_still_running = 1;
	while (threads_still_running == 1) {
	    threads_still_running = 0;
	    PROC_LOCK(p);
	    TAILQ_FOREACH(td, &p->p_threads, td_plist) {
		if (TD_IS_RUNNING(td)) {
		    threads_still_running = 1;
		    break;
		}
	    }
	    PROC_UNLOCK(p);

	    pause_sbt("slsrun", SBT_1MS, 0 , C_HARDCLOCK | C_CATCH);
	}

}


static int
sls_checkpoint(struct proc *p, int mode, struct dump *dump)
{
	int error = 0;

	/* Dump the process state */
	PROC_LOCK(p);

	error = proc_checkpoint(p, &dump->proc, dump->threads);
	if (error != 0) {
	    printf("Error: proc_checkpoint failed with error code %d\n", error);
	    goto sls_checkpoint_out;
	}

	error = vmspace_checkpoint(p, &dump->memory, mode);
	if (error != 0) {
	    printf("Error: vmspace_checkpoint failed with error code %d\n", error);
	    goto sls_checkpoint_out;
	}

	error = fd_checkpoint(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_checkpoint failed with error code %d\n", error);
	    goto sls_checkpoint_out;
	}

	PROC_UNLOCK(p);
sls_checkpoint_out:

	return error;
}

static int 
sls_checkpoint_tofile(struct sls_process *slsp, vm_object_t *objects, 
	int mode, char *filename)
{
    int fd;
    int error;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR | O_CREAT, S_IRWXU);
    fd = curthread->td_retval[0];
    if (error != 0)
	return error;

    error = store_dump(slsp, mode, objects, fd);
    if (error != 0) {
	printf("Error: dumping dump to descriptor failed with %d\n", error);
    }

    kern_close(curthread, fd);

    return error;
}

static int 
sls_checkpoint_tomem(struct sls_process *slsp, vm_object_t *objects, int mode)
{
    struct sls_store_tgt tgt;
    struct vm_map_entry_info *entries;
    int numentries;

    numentries = slsp->slsp_dump->memory.vmspace.nentries;
    entries = slsp->slsp_dump->memory.entries;

    tgt = (struct sls_store_tgt) {
	.type = SLS_FD_MEM,
	.slsp = slsp,
    };
    
    return store_pages(entries, objects, numentries, tgt, mode);
}

static
void sls_checkpoint_one(struct sls_op_args *args)
{
    struct sls_process *slsp;
    vm_object_t *objects;
    size_t numobjects;
    struct proc *p;
    int error;
    int mode;

    p = args->p;
    mode = args->dump_mode;

    slsp = sls_checkpoint_slsp(p, mode);

    /* This causes the process to get detached from its terminal.*/
    sls_stop_proc(p);

    error = sls_checkpoint(p, mode, slsp->slsp_dump);
    if(error != 0) {
	slsp_fini(slsp);
	return;
    }

    objects = sls_shadow(p, &numobjects);

    /* Let the process execute ASAP */
    PROC_LOCK(p);
    kern_psignal(p, SIGCONT);
    PROC_UNLOCK(p);

    if (sls_metadata.slsm_ckptd[p->p_pid] == 1 && mode == SLS_CKPT_FULL)
	sls_compact(objects, numobjects);

    /* Only erase the dump if we are checkpointing to a file */
    if (args->fd_type == SLS_FD_FILE) {
	sls_checkpoint_tofile(slsp, objects, mode, args->filename);
	slsp_fini(slsp);

    } else {
	sls_checkpoint_tomem(slsp, objects, mode);
	TAILQ_INSERT_HEAD(&sls_procs, slsp, slsp_procs);
    }

    if (sls_metadata.slsm_ckptd[p->p_pid] == 1 && mode == SLS_CKPT_DELTA)
	sls_compact(objects, numobjects);

    sls_metadata.slsm_ckptd[p->p_pid] = 1;

}

void
sls_checkpointd(struct sls_op_args *args)
{
    struct timespec tstart, tend;
    int msec_elapsed, msec_left;
    int total_msec = 0;
    int iter;
    int i;

    /* XXX If continuous, register somewhere */

    iter = args->iterations;
    for (i = 0; (iter == 0) || (i < iter); i++) {
	nanotime(&tstart);
	sls_checkpoint_one(args);
	nanotime(&tend);

	msec_elapsed = (tonano(tend) - tonano(tstart)) / (1000 * 1000);
	total_msec += msec_elapsed;
	msec_left = args->interval - msec_elapsed;
	if (msec_left > 0)
	    pause("slscpt", SBT_1MS * msec_left);
    }


    PRELE(args->p);

    free(args->filename, M_SLSMM);
    free(args, M_SLSMM);
    printf("Checkpointing (iterations: %d) for process %d done in %d msec.\n", 
	    iter, args->p->p_pid, total_msec);

    kthread_exit();
}

void
sls_checkpoint_stop(struct proc *p)
{
    return;
}


