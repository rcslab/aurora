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
#include "sls_mosd.h"


static struct dump * 
sls_ckpt_dump(struct proc *p)
{
	struct dump *dump;
	struct thread_info *thread_infos = NULL;
	struct vm_map_entry_info *entries = NULL;
	struct file_info *file_infos = NULL;
	size_t numentries, numthreads, numfiles;

	numthreads = p->p_numthreads;
	numentries = p->p_vmspace->vm_map.nentries;
	numfiles = p->p_fd->fd_files->fdt_nfiles;

	dump = alloc_dump();
	thread_infos = malloc(sizeof(*thread_infos) * numthreads, M_SLSMM, M_WAITOK);
	entries = malloc(sizeof(*entries) * numentries, M_SLSMM, M_WAITOK);
	file_infos = malloc(sizeof(*file_infos) * numfiles, M_SLSMM, M_WAITOK);

	dump->threads = thread_infos;
	dump->memory.entries = entries;
	dump->filedesc.infos = file_infos;

	return dump;
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
	    //printf("(%d)\t\tWaiting\n", p->p_pid);
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
	struct timespec tstart, tend;

	/* Dump the process state */
	PROC_LOCK(p);

	nanotime(&tstart);
	error = proc_ckpt(p, &dump->proc, dump->threads);
	if (error != 0) {
	    printf("Error: proc_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}
	nanotime(&tend);
	sls_log(SLSLOG_PROC, tonano(tend) - tonano(tstart));

	nanotime(&tstart);
	error = vmspace_ckpt(p, &dump->memory, mode);
	if (error != 0) {
	    printf("Error: vmspace_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}
	nanotime(&tend);
	sls_log(SLSLOG_MEM, tonano(tend) - tonano(tstart));

	nanotime(&tstart);
	error = fd_ckpt(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}
	nanotime(&tend);
	sls_log(SLSLOG_FILE, tonano(tend) - tonano(tstart));

	PROC_UNLOCK(p);
sls_ckpt_out:

	return error;
}

static int 
sls_ckpt_tofile(struct dump *dump, struct vmspace *vm, 
	int mode, char *filename)
{
    int fd;
    int error;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR | O_CREAT | O_DIRECT, S_IRWXU);
    fd = curthread->td_retval[0];
    if (error != 0)
	return error;

    error = sls_store(dump, mode, vm, fd);
    if (error != 0) {
	printf("Error: dumping dump to descriptor failed with %d\n", error);
    }

    kern_close(curthread, fd);

    return error;
}

static
void sls_ckpt_one(struct sls_op_args *args, struct sls_process *slsp)
{
    vm_ooffset_t fork_charge, new_charge;
    struct timespec tstart, tend;
    struct vmspace *new_vm;
    struct dump *dump;
    struct proc *p;
    int error;
    int mode;

    p = args->p;
    mode = args->mode;

    dump = sls_ckpt_dump(p);

    SLS_DBG("Dump created\n");

    /* This causes the process to get detached from its terminal.*/
    sls_stop_proc(p);
    SLS_DBG("Process stopped\n");

    nanotime(&tstart);
    error = sls_ckpt(p, mode, dump);
    if(error != 0) {
	free_dump(dump);
	return;
    }
    nanotime(&tend);
    sls_log(SLSLOG_CKPT, tonano(tend) - tonano(tstart));


    /* XXX Create new option for deep deltas */
    /*
    old_vm = slsp->slsp_vm;
    old_charge = slsp->slsp_charge;
    */

    fork_charge = 0;

    nanotime(&tstart);
    new_vm = vmspace_fork(p->p_vmspace, &fork_charge);

    new_charge = fork_charge;
    /* XXX Code only valid for deep deltas, uncomment when ready */
    /*
    if (slsp->slsp_vm == NULL) {
	printf("Error: Shadowing vmspace failed\n");
	slss_fini(slss);
	return;
    }
    */

    if (swap_reserve_by_cred(fork_charge, proc0.p_ucred) == 0) {
	printf("Error: Could not reserve swap space\n");
	vmspace_free(slsp->slsp_vm);
	return;
    }
    SLS_DBG("New vmspace created\n");

    if (slsp->slsp_epoch == 0) {
	slsp->slsp_vm = new_vm;
	slsp->slsp_charge = new_charge;
    }

    nanotime(&tend);
    sls_log(SLSLOG_FORK, tonano(tend) - tonano(tstart));

    /* Let the process execute ASAP */
    PROC_LOCK(p);
    kern_psignal(p, SIGCONT);
    PROC_UNLOCK(p);

    if (slsp->slsp_epoch > 0 && mode == SLS_CKPT_FULL) {
	nanotime(&tstart);
	vmspace_free(new_vm);
	nanotime(&tend);
	sls_log(SLSLOG_COMPACT, tonano(tend) - tonano(tstart));
	SLS_DBG("Full compaction complete\n");
    }

    nanotime(&tstart);
    /* Only erase the dump if we are ckpting to a file */
    switch (args->target) {
    case SLS_FILE:

	sls_ckpt_tofile(dump, slsp->slsp_vm, mode, args->filename);
	free_dump(dump);
	SLS_DBG("Dumped to file\n");
	break;

	/* XXX Bring back SLS_MEM, merge SLS_OSD w/ SLS_FILE */
    default:
	panic("Invalid dump target\n");

    }
    nanotime(&tend);
    sls_log(SLSLOG_DUMP, tonano(tend) - tonano(tstart));


    if (slsp->slsp_epoch > 0 && mode == SLS_CKPT_DELTA) {
	nanotime(&tstart);
	vmspace_free(new_vm);
	nanotime(&tend);
	sls_log(SLSLOG_COMPACT, tonano(tend) - tonano(tstart));
	SLS_DBG("Delta compaction complete\n");
    }

    slsp->slsp_epoch += 1;

}

static char *sls_log_names[] = {"PROC", "MEM", "FILE", "FORK", "COMPACT", "CKPT", "DUMP"};
/*
 * XXX To be refactored later when benchmarking
 */
static int
log_results(void)
{
	int error;
	int logfd;
	char *buf;
	int i, j;
	struct uio auio;
	struct iovec aiov;

	buf = malloc(SLS_LOG_BUFFER * sizeof(*buf), M_SLSMM, M_NOWAIT);
	if (buf == NULL)
	    return ENOMEM;

	error = kern_openat(curthread, AT_FDCWD, "/tmp/sls_benchmark", 
			    UIO_SYSSPACE, 
			    O_RDWR | O_CREAT | O_TRUNC, 
			    S_IRWXU | S_IRWXG | S_IRWXO);
	if (error != 0) {
	    printf("Could not log results, error %d", error);
	    free(buf, M_SLSMM);
	    return error;
	}

	logfd = curthread->td_retval[0];

	for (i = 0; i < slsm.slsm_log_counter; i++) {
	    for (j = 0; j < SLS_LOG_SLOTS; j++) {

		snprintf(buf, 1024, "SLSLOG_%s,%lu\n", sls_log_names[j], slsm.slsm_log[j][i]);

		aiov.iov_base = buf;
		aiov.iov_len = strnlen(buf, 1024);
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_resid = strnlen(buf, 1024);
		auio.uio_segflg = UIO_SYSSPACE;

		error = kern_writev(curthread, logfd, &auio);
		if (error != 0)
		    printf("Writing to file failed with %d\n", error);
	    }

	}

	kern_close(curthread, logfd);
	free(buf, M_SLSMM);

	slsm.slsm_log_counter = 0;
	return 0;
}

void
sls_ckptd(struct sls_op_args *args)
{
    struct timespec tstart, tend;
    struct sls_process *slsp;
    long msec_elapsed, msec_left;
    int iter;
    int i;

    /* HACK */
    sls_osdhack();

    slsp = slsp_add(args->p->p_pid);

    atomic_store_int(&slsp->slsp_status, 1);

    SLS_DBG("Process active\n");

    iter = args->iterations;
    for (i = 0; (iter == 0) || (i < iter); i++) {

	if (atomic_load_int(&slsp->slsp_status) == 0)
	    break;

	nanotime(&tstart);
	sls_ckpt_one(args, slsp);
	nanotime(&tend);
	SLS_DBG("Checkpointed process once\n");

	slsp->slsp_epoch += 1;

	msec_elapsed = (tonano(tend) - tonano(tstart)) / (1000 * 1000);
	msec_left = args->interval - msec_elapsed;
	if (msec_left > 0)
	    pause_sbt("slscpt", SBT_1MS * msec_left, 0, C_HARDCLOCK | C_CATCH);

	SLS_DBG("Woke up\n");
	sls_log_new();
    }

    SLS_DBG("Stopped checkpointing\n");

    PRELE(args->p);
    log_results();

    printf("ckpting (iterations: %d) for process %d done.\n", 
	    iter, args->p->p_pid);
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


