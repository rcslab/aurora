#include "_slsmm.h"

#include "dump.h"
#include "memckpt.h"
#include "slsmm.h"
#include "backends/desc.h"
#include "backends/fileio.h"
#include "hash.h"
#include "fd.h"
#include "bufc.h"
#include "vnhash.h"

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

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");

int pid_checkpointed[SLS_MAX_PID];
vm_object_t pid_shadows[SLS_MAX_PID];
int time_to_die;

static struct cdev *slsmm_dev;

/* XXX Temporary benchmarking code */
long  sls_log[9][SLS_LOG_ENTRIES];
int sls_log_counter = 0;


static int
slsmm_dump(struct proc *p, struct sls_desc desc, int mode)
{
	int error = 0;
	struct thread_info *thread_infos = NULL;
	struct vm_map_entry_info *entries = NULL;
	struct file_info *file_infos = NULL;
	struct dump *dump;
	vm_object_t *objects;
	vm_map_t vm_map = &p->p_vmspace->vm_map;
	struct timespec tstart, tsuspend, tproc,
			tfd, tmem, tresume, tflush;
	int threads_still_running;
	struct thread *td;

	/*
	* XXX Find a better way to do allocations, we cannot keep doing it
	* piecemeal. If we find a clever dump encoding, I predict it will be
	* very elegant.
	*/

	dump = alloc_dump();
	if (!dump)
	    return ENOMEM;

	thread_infos = malloc(sizeof(struct thread_info) * p->p_numthreads,
		M_SLSMM, M_NOWAIT);
	entries = malloc(sizeof(struct vm_map_entry_info) * vm_map->nentries,
		M_SLSMM, M_NOWAIT);
	objects = malloc(sizeof(vm_object_t) * vm_map->nentries,
		M_SLSMM, M_NOWAIT);
	file_infos = malloc(sizeof(struct file_info) * p->p_fd->fd_files->fdt_nfiles,
		M_SLSMM, M_NOWAIT);


	if (!thread_infos || !objects || !entries || !file_infos) {
	    error = ENOMEM;
	    goto slsmm_dump_cleanup;
	}

	dump->threads = thread_infos;
	dump->memory.entries = entries;
	dump->filedesc.infos = file_infos;


	nanotime(&tstart);

	/* Suspend the process */
	PROC_LOCK(p);
	/*
	* This causes the process to get detached from
	* its terminal, unfortunately. We can solve this
	* by using a different kind of STOP (e.g. breakpoint).
	*/
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

	PROC_LOCK(p);

	nanotime(&tsuspend);

	/* Dump the process states */
	error = proc_checkpoint(p, &dump->proc, thread_infos);
	if (error != 0) {
	    printf("Error: proc_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&tproc);

	/* Prepare the vmspace for dump */
	error = vmspace_checkpoint(p, &dump->memory, objects, mode);
	if (error != 0) {
	    printf("Error: vmspace_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&tmem);

	error = fd_checkpoint(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&tfd);

	/* Let the process execute ASAP */
	kern_psignal(p, SIGCONT);
	PROC_UNLOCK(p);

	nanotime(&tresume);

	error = store_dump(p, dump, objects, mode, &desc);
	if (error != 0) {
	    printf("Error: dumping dump to descriptor failed with %d\n", error);
	    goto slsmm_dump_cleanup;
	}

	/* 
	 * For full dumps, collapse the chain here to avoid slowdowns while the
	 * process is stopped
	 */
	if (mode == SLSMM_CKPT_FULL && pid_checkpointed[p->p_pid] == 1)
	    vmspace_compact(p);

	pid_checkpointed[p->p_pid] = 1;

	nanotime(&tflush);

	/*
	printf("suspend		%ldns\n",  tonano(tsuspend) - tonano(tstart));
	printf("proc_ckpt	%ldns\n", tonano(tproc) - tonano(tsuspend));
	printf("vmspace	\t%ldns\n", tonano(tmem) - tonano(tproc));
	printf("fd		%ldns\n", tonano(tfd) - tonano(tmem));
	printf("total_suspend	%ldns\n", tonano(tresume) - tonano(tfd));
	printf("flush_disk	%ldns\n", tonano(tflush) - tonano(tresume));
	*/

	sls_log[0][sls_log_counter] = tonano(tsuspend) - tonano(tstart);
	sls_log[1][sls_log_counter] = tonano(tproc) - tonano(tsuspend);
	sls_log[2][sls_log_counter] = tonano(tmem) - tonano(tproc);
	sls_log[3][sls_log_counter] = tonano(tfd) - tonano(tmem);
	sls_log[4][sls_log_counter] = tonano(tresume) - tonano(tfd);
	sls_log[5][sls_log_counter] = tonano(tflush) - tonano(tresume);



    slsmm_dump_cleanup:
	destroy_desc(desc);
	free(objects, M_SLSMM);
	free_dump(dump);


	return error;
}

static int
slsmm_restore(struct proc *p, struct dump *dump)
{
	int error = 0;

	/*
	* Send the signal before locking, otherwise
	* the thread state flags don't get updated.
	*
	* XXX We don't actually need that, right? We're overwriting ourselves,
	* so we definitely don't want to stop.
	*/
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);


	error = proc_restore(p, &dump->proc, dump->threads);
	if (error != 0) {
	    printf("Error: reg_restore failed with error code %d\n", error);
	    goto slsmm_restore_out;
	}

	error = vmspace_restore(p, &dump->memory);
	if (error != 0) {
	    printf("Error: vmspace_restore failed with error code %d\n", error);
	    goto slsmm_restore_out;
	}

	error = fd_restore(p, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_restore failed with error code %d\n", error);
	    goto slsmm_restore_out;
	}

	kern_psignal(p, SIGCONT);
	PROC_UNLOCK(p);

slsmm_restore_out:

	return error;
}

struct sls_restored_args {
    struct proc *p;
    char *filename;
    int fd_type;
};

/* XXX Check for leaks */
static void
sls_restored(struct sls_restored_args *args)
{
    struct proc *p;
    int error;
    struct sls_desc desc;
    struct dump *dump;

    error = setup_hashtable();
    if (error != 0)
	kthread_exit();

    p = args->p;
    /* XXX Error handling */
    desc = create_desc((long) args->filename, args->fd_type);

    dump = alloc_dump();
    if (dump == NULL) {
	printf("Error: dump not created\n");
	goto restored_out;
    }
    load_dump(dump, &desc);
	
    error = slsmm_restore(p, dump);
    if (error != 0)
	printf("Error: slsmm_restore failed with %d\n", error);

restored_out:
    free(args, M_SLSMM);
    destroy_desc(desc);
    free_dump(dump);
    cleanup_hashtable();
    PRELE(p);

    dev_relthread(slsmm_dev, 1);
    kthread_exit();
}

static chan_t *chan;
static volatile int request_count, flush_count;

static int
slsmm_ioctl(struct cdev *dev, u_long cmd, caddr_t data,
	int flag __unused, struct thread *td)
{
	int error = 0;
	struct dump_param *dparam = NULL;
	struct compose_param *cparam = NULL;
	struct restore_param *rparam = NULL;
	struct proc *p = NULL;
	struct sls_desc *descs = NULL;
	struct sls_desc desc;
	struct sls_chan_args *chan_args;
	struct sls_restored_args *restored_args;
	char *snap_name;
	size_t snap_name_len;
	int *fds = NULL;
	int fd_type;
	int ndescs;
	struct dump *dump = NULL;
	int i;

	/*
	* Get a hold of the process to be
	* checkpointed, bringing its thread stack to memory
	* and preventing it from exiting, to avoid race
	* conditions.
	*/
	switch (cmd) {
	    /* 
	     * XXX rn we want it to be self-contained bc the code is a mess.
	     * factor out the error handling later, it's being repeated for
	     * every possible error. 
	     */
	    case SLSMM_DUMP:

		dparam = (struct dump_param *) data;
		fd_type = dparam->fd_type;

		if (fd_type <= SLSMM_FD_INVALID_LOW || 
		    fd_type >= SLSMM_FD_INVALID_HIGH) {
		    printf("Error: Invalid checkpoint fd type %d\n", fd_type);
		    return EINVAL;
		}

		error = pget(dparam->pid, PGET_WANTREAD, &p);
		if (error != 0) {
		    printf("Error: pget failed with %d\n", error);
		    break;
		}

		snap_name_len = MIN(dparam->len, 1024);
		snap_name = malloc(snap_name_len, M_SLSMM, M_NOWAIT);
		if (snap_name == NULL) {
		    error = ENOMEM;
		    PRELE(p);
		    break;
		}

		error = copyin(dparam->name, snap_name, snap_name_len);
		if (error != 0) {
		    printf("Error: Copying name failed with %d\n", error);
		    free(snap_name, M_SLSMM);
		    PRELE(p);
		    break;
		}
		snap_name[snap_name_len] = '\0';


		if (dparam->async != 0) {
		    chan_args = malloc(sizeof(struct sls_chan_args), M_SLSMM, M_NOWAIT);
		    if (chan_args == NULL) {
			error = ENOMEM;
			free(snap_name, M_SLSMM);
			PRELE(p);
			break;
		    }

		    chan_args->filename = snap_name;
		    chan_args->p = p;
		    chan_args->dump_mode = dparam->dump_mode;
		    chan_args->request_id = request_count++;
		    chan_args->fd_type = dparam->fd_type;

		    chan_send(chan, chan_args);

		    break;
		}


		/* Now we are at the sync path */
		desc = create_desc((long) dparam->name, dparam->fd_type);
		if (desc.type == DESCRIPTOR_SIZE) { 
		    printf("Error: invalid descriptor\n");
		    error = EINVAL;
		    free(snap_name, M_SLSMM);
		    PRELE(p);
		    break;
		}

		error = slsmm_dump(p, desc, dparam->dump_mode);
		free(snap_name, M_SLSMM);
		PRELE(p);

		break;

	    /* XXX turn into a sysctl */
	    case SLSMM_FLUSH_COUNT:
		*((int *) data) = flush_count;
		break;


	    /* Only works for restoring the current process */
	    case SLSMM_RESTORE:
		rparam = (struct restore_param *) data;
		fd_type = rparam->fd_type;

		if (fd_type <= SLSMM_FD_INVALID_LOW || 
		    fd_type >= SLSMM_FD_INVALID_HIGH) {
		    printf("Error: Invalid checkpoint fd type %d\n", fd_type);
		    return EINVAL;
		}

		error = pget(rparam->pid, PGET_WANTREAD, &p);
		if (error != 0) {
		    printf("Error: pget failed with %d\n", error);
		    return EINVAL;
		}


		snap_name_len = MIN(rparam->len, 1024);
		snap_name = malloc(snap_name_len, M_SLSMM, M_NOWAIT);
		if (snap_name == NULL) {
		    printf("Error: allocating snapshot name failed\n");
		    PRELE(p);
		    return ENOMEM;
		}
		
		error = copyin(rparam->name, snap_name, snap_name_len);
		if (error != 0) {
		    printf("Error: Copying name failed with %d\n", error);
		    free(snap_name, M_SLSMM);
		    PRELE(p);
		    return error;
		}
		snap_name[snap_name_len] = '\0';

		restored_args = malloc(sizeof(*restored_args), M_SLSMM, M_NOWAIT);
		if (restored_args == NULL) {
		    printf("Error: Allocating restored_args failed\n");
		    error = ENOMEM;
		    PRELE(p);
		    break;
		}

		restored_args->p = p;
		restored_args->filename = snap_name;
		restored_args->fd_type = rparam->fd_type;

		sls_restored(restored_args);

		error = kthread_add((void(*)(void *))sls_restored, restored_args, 
			    p, NULL, 0, 0, "sls_restored");
		kern_thr_exit(curthread);

		break;

	    /*
	     * XXX fix up so that it works with all descriptors, this will be how
	     * we import from / export to the OSD and memory.
	     */
	    case SLSMM_COMPOSE:
		cparam = (struct compose_param *) data;

		/* XXX Only user-provided fds can be used for now */
		if (cparam->fd_type != SLSMM_FD_FD || cparam->outfd_type != SLSMM_FD_FD)
		    return EINVAL;

		/* XXX error handling */
		fds = malloc(sizeof(*fds) * cparam->nfds, M_SLSMM, M_NOWAIT);
		if (fds == NULL) {
		    return ENOMEM;
		}

		error = copyin(cparam->fds, fds, sizeof(int) * cparam->nfds);
		if (error != 0) {
		    printf("Error: Copying fds failed with %d \n", error);
		    goto slsmm_compose_out;
		}

		ndescs = cparam->nfds;
		descs = malloc(sizeof(struct sls_desc) * ndescs, M_SLSMM, M_NOWAIT);
		if (descs == NULL) {
		    printf("Error: Allocating descriptors failed\n");
		    error = ENOMEM;
		    goto slsmm_compose_out;
		}

		/* XXX error handling */
		for (i = 0; i < ndescs; i++) {
		    descs[i] = create_desc((long) fds[i], cparam->fd_type);
		}

		desc = create_desc((long) cparam->outfd, cparam->outfd_type);

		error = setup_hashtable();
		/* error handling */
		dump = compose_dump(descs, ndescs);
		/* XXX wrong error handling */
		if (!dump)
		    goto slsmm_compose_out;

		/* XXX error handling */
		/* XXX Change the mode code */
		error = store_dump(NULL, dump, NULL, SLSMM_CKPT_FULL, &desc);


slsmm_compose_out:
		cleanup_hashtable();
		free_dump(dump);
		free(descs, M_SLSMM);
		free(fds, M_SLSMM);
		break;

	}

	return error;
}

static struct cdevsw slsmm_cdevsw = {
    .d_version = D_VERSION,
    .d_ioctl = slsmm_ioctl,
};


/*
 * XXX Async is temporarily our benchmarking call,
 * we can split these later.
 */
static int
chan_handler()
{
    const int interval = 1000;
    const int iterations = 10;
    struct sls_chan_args *chan_args= NULL;
    struct timespec start, end, total;
    struct sls_desc desc;
    struct proc *p = NULL;
    int msec;
    int error;
    int request_id;
    int dump_mode;
    char *filename;
    int fd_type;
    int i;

    while (time_to_die == 0) {
	error = 0;

	chan_args = chan_recv(chan);
	if (time_to_die != 0) {
	    free(chan_args, M_SLSMM);
	    break;
	}

	p = chan_args->p;
	dump_mode = chan_args->dump_mode;
	request_id = chan_args->request_id;
	filename = chan_args->filename;
	fd_type = chan_args->fd_type;

	printf("Starting benchmarking...\n");

	desc = create_desc((long) filename, fd_type);
	if (desc.type == DESCRIPTOR_SIZE) {
	    printf("Invalid descriptor\n");
	    PRELE(p);
	    free(chan_args, M_SLSMM);
	    continue;
	} else {
	    for (i = 0; i < iterations; i++) {
		nanotime(&start);

		sls_log_counter = i;
		sls_log[7][sls_log_counter] = 0;
		error = slsmm_dump(p, desc, dump_mode);
		if(error != 0) {
		    printf("dump failed with %d\n", error);
		    break;
		}

		nanotime(&end);

		sls_log[6][sls_log_counter] = tonano(end) - tonano(start);
		/* 
		 * Each iteration of this loop is 100ms regardless
		 * of the time it took to checkpoint. If it wook
		 * more than 100ms, then don't sleep at all, but
		 * this should be rare enough.
		 */
		msec = (tonano(end) - tonano(start)) / (1000 * 1000); 
		if (msec < interval) {
		    pause("sls", (hz * (interval - msec)) / 1000);
		}

		nanotime(&total);
	    }
	}

	PRELE(p);

	/* If the error is not 0, we messed up somewhere in the loop */
	if (error == 0)
	    error = kern_openat(curthread, AT_FDCWD, "/tmp/sls_benchmark", 
				UIO_SYSSPACE, 
				O_RDWR | O_CREAT | O_TRUNC, 
				S_IRWXU | S_IRWXG | S_IRWXO);
	if (error != 0) {
	    printf("Could not log results, error %d", error);
	    free(chan_args, M_SLSMM);
	    continue;
	}

	int logfd = curthread->td_retval[0];

	char buffer[1024];

	for (i = 0; i < iterations; i++) {
	    for (int j = 0; j < 9; j++) {
		struct uio auio;
		struct iovec aiov;

		snprintf(buffer, 1024, "%u,%lu\n", j, sls_log[j][i]);

		aiov.iov_base = buffer;
		aiov.iov_len = strnlen(buffer, 1024);
		auio.uio_iov = &aiov;
		auio.uio_iovcnt = 1;
		auio.uio_resid = strnlen(buffer, 1024);
		auio.uio_segflg = UIO_SYSSPACE;

		error = kern_writev(curthread, logfd, &auio);
		if (error)
		    printf("Writing to file failed with %d\n", error);
	    }

	}
	kern_close(curthread, logfd);

	printf("Benchmarking done.\n");


	flush_count++;
	free(chan_args->filename, M_SLSMM);
	free(chan_args, M_SLSMM);
    }


    chan_fini(chan);
    kproc_exit(0);

}


static int
SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
    int error = 0;
    int i;
    
    switch (inEvent) {
	case MOD_LOAD:
	    printf("SLS Loaded.\n");
	    time_to_die = 0;


	    backends_init();

	    for (i = 0; i < SLS_MAX_PID; i++)
		pid_checkpointed[i] = 0;

	    error = setup_vnhash();
	    if (error != 0)
		return error;

	    chan = chan_init(64);
	    request_count = 0;
	    flush_count = 0;
	    if (chan == NULL) {
		uprintf("chan init failed\n");
	    }
	    else uprintf("%zx\n", chan->capacity);

	    kproc_create((void (*)(void*)) chan_handler, NULL, NULL, 0, 0, "chan_handler"); 

	    slsmm_dev = make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "slsmm");

	    break;

	case MOD_UNLOAD:

	    time_to_die = 1;

	    printf("SLS Unloaded.\n");
	    destroy_dev(slsmm_dev);

	    mtx_lock(&chan->mu);
	    cv_signal(&chan->r_cv);
	    mtx_unlock(&chan->mu);

	    cleanup_vnhash();
	    backends_fini();

	    break;
	default:
	    error = EOPNOTSUPP;
	    break;
    }
    return error;
}

static moduledata_t moduleData = {
    "slsmm",
    SLSMMHandler,
    NULL
};


DECLARE_MODULE(slsmm_kmod, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
