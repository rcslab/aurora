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

#include <machine/atomic.h>
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
#include "sls_mosd.h"

static void
sls_ckpt_dump_init(struct proc *p, struct dump *dump)
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

static struct sls_snapshot *
sls_ckpt_slss(struct proc *p, int mode)
{
    struct sls_snapshot *slss;
    slss = slss_init(p, mode);
    sls_ckpt_dump_init(p, slss->slss_dump);

    return slss;
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
sls_ckpt(struct proc *p, int mode, struct dump *dump)
{
	int error = 0;

	/* Dump the process state */
	PROC_LOCK(p);

	error = proc_ckpt(p, &dump->proc, dump->threads);
	if (error != 0) {
	    printf("Error: proc_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}

	error = vmspace_ckpt(p, &dump->memory, mode);
	if (error != 0) {
	    printf("Error: vmspace_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}

	error = fd_ckpt(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}

	PROC_UNLOCK(p);
sls_ckpt_out:

	return error;
}

static int 
sls_ckpt_tofile(struct sls_snapshot *slss, struct vmspace *vm, 
	int mode, char *filename)
{
    int fd;
    int error;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR | O_CREAT | O_DIRECT, S_IRWXU);
    fd = curthread->td_retval[0];
    if (error != 0)
	return error;

    error = store_dump(slss, mode, vm, fd);
    if (error != 0) {
	printf("Error: dumping dump to descriptor failed with %d\n", error);
    }

    kern_close(curthread, fd);

    return error;
}

/* Dirty hack for breaking down checkpoint time */
#define SLS_SHADOW_TIME slsm.slsm_log[0][0]
#define SLS_CKPT_TIME slsm.slsm_log[1][0]
#define SLS_DUMP_TIME slsm.slsm_log[2][0]
#define SLS_COMPACT_TIME slsm.slsm_log[3][0]

static
void sls_ckpt_one(struct sls_op_args *args, struct sls_process *slsp)
{
    vm_ooffset_t fork_charge, old_charge;
    struct timespec tstart, tend;
    struct vmspace *old_vm;
    struct sls_snapshot *slss;
    struct proc *p;
    int error;
    int mode;

    p = args->p;
    mode = args->mode;

    slss = sls_ckpt_slss(p, mode);

    /* This causes the process to get detached from its terminal.*/
    sls_stop_proc(p);

    nanotime(&tstart);
    error = sls_ckpt(p, mode, slss->slss_dump);
    if(error != 0) {
	slss_fini(slss);
	return;
    }
    nanotime(&tend);
    SLS_CKPT_TIME += (tonano(tend) - tonano(tstart)) / (1000 * 1000);


    old_vm = slsp->slsp_vm;
    old_charge = slsp->slsp_charge;

    fork_charge = 0;

    nanotime(&tstart);
    slsp->slsp_vm = vmspace_fork(p->p_vmspace, &fork_charge);
    nanotime(&tend);
    SLS_SHADOW_TIME += (tonano(tend) - tonano(tstart)) / (1000 * 1000);

    slsp->slsp_charge = fork_charge;
    if (slsp->slsp_vm== NULL) {
	printf("Error: Shadowing vmspace failed\n");
	slss_fini(slss);
	return;
    }

    if (swap_reserve_by_cred(fork_charge, proc0.p_ucred) == 0) {
	printf("Error: Could not reserve swap space\n");
	vmspace_free(slsp->slsp_vm);
	return;
    }

    /* Let the process execute ASAP */
    PROC_LOCK(p);
    kern_psignal(p, SIGCONT);
    PROC_UNLOCK(p);

    if (slsp->slsp_ckptd == 1 && mode == SLS_CKPT_FULL) {
	nanotime(&tstart);
	vmspace_free(old_vm);
	nanotime(&tend);
	SLS_COMPACT_TIME += (tonano(tend) - tonano(tstart)) / (1000 * 1000);
    }

    nanotime(&tstart);
    /* Only erase the dump if we are ckpting to a file */
    switch (args->target) {
    case SLS_FILE:

	sls_ckpt_tofile(slss, slsp->slsp_vm, mode, args->filename);
	slss_fini(slss);
	break;

    case SLS_MEM:
	LIST_INSERT_HEAD(&slsm.slsm_snaplist, slss, slss_snaps);
	LIST_INSERT_HEAD(&slsp->slsp_snaps, slss, slss_procsnaps);
	break;

    case SLS_OSD:
	osd_dump(slss, slsp->slsp_vm, mode);
	break;

    default:
	panic("Invalid dump target\n");

    }
    nanotime(&tend);
    SLS_DUMP_TIME += (tonano(tend) - tonano(tstart)) / (1000 * 1000);

    if (slsp->slsp_ckptd == 1 && mode == SLS_CKPT_DELTA) {
	nanotime(&tstart);
	vmspace_free(old_vm);
	nanotime(&tend);
	SLS_COMPACT_TIME += (tonano(tend) - tonano(tstart)) / (1000 * 1000);
    }

    slsp->slsp_ckptd = 1;

}


void
sls_ckptd(struct sls_op_args *args)
{
    struct timespec tstart, tend;
    struct sls_process *slsp;
    long msec_elapsed, msec_left;
    long total_msec = 0;
    int iter;
    int i;

    /* HACK */
    sls_osdhack();

    SLS_SHADOW_TIME = 0;
    SLS_CKPT_TIME = 0;
    SLS_DUMP_TIME = 0;
    SLS_COMPACT_TIME = 0;

    slsp = slsp_add(args->p->p_pid);

    atomic_set_int(&slsp->slsp_active, 1);

    /* XXX Look at possible race conditions more closely */

    iter = args->iterations;
    for (i = 0; (iter == 0) || (i < iter); i++) {

	if (atomic_load_int(&slsp->slsp_active) == 0) {
	    break;
	}

	nanotime(&tstart);
	sls_ckpt_one(args, slsp);
	nanotime(&tend);

	slsp->slsp_epoch += 1;

	msec_elapsed = (tonano(tend) - tonano(tstart)) / (1000 * 1000);
	total_msec += msec_elapsed;
	msec_left = args->interval - msec_elapsed;
	if (msec_left > 0)
	    pause_sbt("slscpt", SBT_1MS * msec_left, 0, C_HARDCLOCK | C_CATCH);
    }

    atomic_set_int(&slsp->slsp_active, 0);

    PRELE(args->p);

    printf("ckpting (iterations: %d) for process %d done.\n", 
	    iter, args->p->p_pid);
    printf("Total time needed: %ld msec.\n", total_msec);
    printf("Total time needed for checkpoints: %ld msec.\n", SLS_CKPT_TIME);
    printf("Total time needed for shadowing: %ld msec.\n", SLS_SHADOW_TIME);
    printf("Total time needed for dumping: %ld msec.\n", SLS_DUMP_TIME);
    free(args->filename, M_SLSMM);
    free(args, M_SLSMM);

    kthread_exit();
}

void
sls_ckpt_stop(struct proc *p)
{
    /* XXX Implement */
    return;
}


