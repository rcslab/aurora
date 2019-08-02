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

#define SLS_SIG(p, sig) \
	PROC_LOCK(p); \
	kern_psignal(p, sig); \
	PROC_UNLOCK(p); \

#define SLS_CONT(p) SLS_SIG(p, SIGCONT)
    
#define SLS_STOP(p) SLS_SIG(p, SIGSTOP)

#define SLS_PROCALIVE(proc) \
    (((proc)->p_state != PRS_ZOMBIE) && !((proc)->p_flag & P_WEXIT))

#define SLS_RUNNABLE(proc, slsp) \
    (atomic_load_int(&slsp->slsp_status) != 0 && \
    SLS_PROCALIVE(proc))

SDT_PROVIDER_DEFINE(sls);

SDT_PROBE_DEFINE(sls, , , stop_entry);
SDT_PROBE_DEFINE(sls, , , stop_exit);

void sls_stop_proc(struct proc *p);
void sls_ckpt_one(struct sls_op_args *args, struct sls_process *slsp);
int sls_ckpt_tofile(struct sls_process *slsp, int mode, char *filename);
int sls_ckpt(struct proc *p, int mode, struct sls_process *slsp);

void
sls_stop_proc(struct proc *p)
{
	SDT_PROBE0(sls, , , stop_entry);
	int threads_still_running;
	struct thread *td;
	
	SLS_STOP(p);

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
	    if(!SLS_PROCALIVE(p)) {
		SLS_DBG("Proc trying to die, exiting stop\n");
		PROC_UNLOCK(p);
		break;
	    }
	    PROC_UNLOCK(p);
	    pause_sbt("slsrun", 50 * SBT_1US, 0 , C_HARDCLOCK | C_CATCH);
	}
        SDT_PROBE0(sls, , , stop_exit);
}


int
sls_ckpt(struct proc *p, int mode, struct sls_process *slsp)
{
	int error = 0;
	struct timespec tstart, tend;

	/* Dump the process state */
	PROC_LOCK(p);
	if (!SLS_PROCALIVE(p)) {
	    SLS_DBG("proc trying to die, exiting ckpt\n");
	    error = ESRCH;
	    goto sls_ckpt_out;
	}

	nanotime(&tstart);
	error = sls_proc_ckpt(p, slsp->slsp_ckptbuf);
	if (error != 0) {
	    SLS_DBG("Error: proc_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}
	nanotime(&tend);
	sls_log(SLSLOG_PROC, tonano(tend) - tonano(tstart));

	nanotime(&tstart);
	error = sls_filedesc_ckpt(p, slsp->slsp_ckptbuf);
	if (error != 0) {
	    SLS_DBG("Error: fd_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}
	nanotime(&tend);
	sls_log(SLSLOG_FILE, tonano(tend) - tonano(tstart));

	nanotime(&tstart);
	error = sls_vmspace_ckpt(p, slsp->slsp_ckptbuf, mode);
	if (error != 0) {
	    SLS_DBG("Error: vmspace_ckpt failed with error code %d\n", error);
	    goto sls_ckpt_out;
	}
	nanotime(&tend);
	sls_log(SLSLOG_MEM, tonano(tend) - tonano(tstart));


sls_ckpt_out:
	PROC_UNLOCK(p);

	return error;
}

int 
sls_ckpt_tofile(struct sls_process *slsp, int mode, char *filename)
{
    SLS_DBG("Flushing to file\n");
    int fd;
    int error;

    error = kern_openat(curthread, AT_FDCWD, filename, 
	UIO_SYSSPACE, O_RDWR | O_CREAT | O_DIRECT, S_IRWXU);
    fd = curthread->td_retval[0];
    if (error != 0)
	return error;

    SLS_DBG("Dumping to %s\n", filename);

    error = sls_dump(slsp, mode, fd);
    if (error != 0) {
	printf("Error: dumping dump to descriptor failed with %d\n", error);
    }

    kern_close(curthread, fd);

    SLS_DBG("Flushing done to file\n");
    return error;
}

void 
sls_ckpt_one(struct sls_op_args *args, struct sls_process *slsp)
{
    vm_ooffset_t fork_charge, new_charge;
    struct timespec tstart, tend;
    struct vmspace *new_vm;
    struct proc *p;
    int error = 0;
    int mode;

    p = args->p;
    mode = args->mode;

    SLS_DBG("Dump created\n");

    /* This causes the process to get detached from its terminal.*/
    sls_stop_proc(p);
    SLS_DBG("Process stopped\n");

    nanotime(&tstart);
    error = sls_ckpt(p, mode, slsp);
    if(error != 0) {
	SLS_DBG("Checkpointing failed\n");
	SLS_CONT(p);

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
    SLS_CONT(p);

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

	sls_ckpt_tofile(slsp, mode, args->filename);
	/* XXX Free the sbuf after that */
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
    SLS_DBG("Checkpointed process once\n");
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

    slsp = slsp_add(args->p->p_pid);

    atomic_store_int(&slsp->slsp_status, 1);

    SLS_DBG("Process active\n");

    iter = args->iterations;
    for (i = 0; (iter == 0) || (i < iter); i++) {

	if (!SLS_RUNNABLE(args->p, slsp)) {
	    SLS_DBG("Process no longer runnable\n");
	    break;
	}

	nanotime(&tstart);
	sls_ckpt_one(args, slsp);
	nanotime(&tend);

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


