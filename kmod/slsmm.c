#include "_slsmm.h"
#include "cpuckpt.h"
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

MALLOC_DEFINE(M_SLSMM, "slsmm", "S L S M M");


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
	struct timespec t_suspend, t_suspend_complete, t_proc_ckpt,
			t_fd_ckpt, t_vmspace_ckpt, t_resumed, t_flush_disk;

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


	nanotime(&t_suspend);

	/* Suspend the process */
	PROC_LOCK(p);
	/*
	* Send the signal before locking, otherwise
	* the thread state flags don't get updated.
	*
	* This causes the process to get detached from
	* its terminal, unfortunately. We can solve this
	* by using a different kind of STOP (e.g. breakpoint).
	*/
	kern_psignal(p, SIGSTOP);

	nanotime(&t_suspend_complete);

	/* Dump the process states */
	error = proc_checkpoint(p, &dump->proc, thread_infos);
	if (error != 0) {
	    printf("Error: proc_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&t_proc_ckpt);

	/* Prepare the vmspace for dump */
	error = vmspace_checkpoint(p->p_vmspace, &dump->memory, objects, mode);
	if (error != 0) {
	    printf("Error: vmspace_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&t_vmspace_ckpt);

	error = fd_checkpoint(p->p_fd, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&t_fd_ckpt);

	/* Let the process execute ASAP */
	kern_psignal(p, SIGCONT);
	PROC_UNLOCK(p);

	nanotime(&t_resumed);

	error = store_dump(dump, objects, mode, desc);
	if (error != 0) {
	    printf("Error: dumping dump to descriptor failed with %d\n", error);
	    goto slsmm_dump_cleanup;
	}

	nanotime(&t_flush_disk);

	uprintf("suspend		%ldns\n", t_suspend_complete.tv_nsec-t_suspend.tv_nsec);
	uprintf("proc_ckpt	%ldns\n", t_proc_ckpt.tv_nsec-t_suspend_complete.tv_nsec);
	uprintf("fd_ckpt		%ldns\n", t_vmspace_ckpt.tv_nsec-t_proc_ckpt.tv_nsec);
	uprintf("vmspace		%ldns\n", t_fd_ckpt.tv_nsec-t_vmspace_ckpt.tv_nsec);
	uprintf("total_suspend	%ldns\n", t_resumed.tv_nsec-t_suspend.tv_nsec);
	uprintf("flush_disk	%ldns\n", t_flush_disk.tv_nsec-t_resumed.tv_nsec);

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
    desc = create_desc(args->filename, args->fd_type);
    //free(args, M_SLSMM);

    dump = alloc_dump();
    if (dump == NULL) {
	/* XXX error handling */
	printf("Error: dump not created\n");
    }
    load_dump(dump, desc);
    destroy_desc(desc);

	
    error = slsmm_restore(p, dump);
    if (error != 0)
	printf("Error: slsmm_restore failed with %d\n", error);

    free_dump(dump);
    cleanup_hashtable();
    PRELE(p);

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
		/* XXX Only files for now */
		desc = create_desc(dparam->name, dparam->fd_type);
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
		/* XXX Merge first steps with SLSMM_DUMP, it's the same model */
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
		    break;
		}


		snap_name_len = MIN(rparam->len, 1024);
		snap_name = malloc(snap_name_len, M_SLSMM, M_NOWAIT);
		if (snap_name == NULL) {
		    /* XXX error handling */
		}
		
		error = copyin(rparam->name, snap_name, snap_name_len);
		if (error != 0) {
		    printf("Error: Copying name failed with %d\n", error);
		    /* XXX error handling */
		    break;
		}
		snap_name[snap_name_len] = '\0';

		/* XXX Settle on sync/async version (probably async) */
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

	    /* XXX fix up so that it works with files */
	    /* XXX fix up so that it works with all descriptors */
	    case SLSMM_COMPOSE:

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

		/* XXX initialize descriptors from fds */

		dump = compose_dump(descs, ndescs);
		if (!dump)
		    goto slsmm_compose_out;

		/* Pipe result to output */

slsmm_compose_out:
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
static struct cdev *slsmm_dev;

static int
chan_handler()
{
    struct sls_chan_args *chan_args= NULL;
    struct sls_desc desc;
    struct proc *p = NULL;
    int error;
    int request_id;
    int dump_mode;
    char *filename;
    int fd_type;

    for (;;) {
	chan_args = chan_recv(chan);

	p = chan_args->p;
	dump_mode = chan_args->dump_mode;
	request_id = chan_args->request_id;
	filename = chan_args->filename;
	fd_type = chan_args->fd_type;

	printf("request %d\n", request_id);

	printf("fd_type is %d\n", fd_type);
	desc = create_desc(filename, fd_type);
	printf("descriptor type is %d\n", desc.type);
	if (desc.type == DESCRIPTOR_SIZE) 
	    printf("Invalid descriptor\n");
	else
	    error = slsmm_dump(p, desc, dump_mode);

	PRELE(p);

	flush_count ++;
	printf("%d\n", flush_count);
	free(chan_args, M_SLSMM);
    }
}

static int
SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
    int error = 0;
    switch (inEvent) {
	case MOD_LOAD:
	    printf("Loaded\n");
	    backends_init();

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


	    printf("Unloaded\n");
	    destroy_dev(slsmm_dev);

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
