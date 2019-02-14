#include "_slsmm.h"
#include "cpuckpt.h"
#include "dump.h"
#include "memckpt.h"
#include "slsmm.h"
#include "fileio.h"
#include "hash.h"
#include "fd.h"

#include <sys/types.h>

#include <sys/conf.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
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

	/*
	* Send the signal before locking, otherwise
	* the thread state flags don't get updated.
	*
	* This causes the process to get detached from
	* its terminal, unfortunately. We can solve this
	* by using a different kind of STOP (e.g. breakpoint).
	*/
	kern_psignal(p, SIGSTOP);

	/* Suspend the process */
	PROC_LOCK(p);

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

	/* XXX Maybe don't do it here? We'll see when benchmarking if it's slow */
	error = fd_checkpoint(p->p_fd, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_checkpoint failed with error code %d\n", error);
	    goto slsmm_dump_cleanup;
	}
	nanotime(&t_fd_ckpt);


	PROC_UNLOCK(p);

	/* Let the process execute ASAP */
	kern_psignal(p, SIGCONT);

	nanotime(&t_resumed);

	error = store_dump(dump, objects, mode, desc);
	if (error != 0) {
	    printf("Error: dumping dump to descriptor failed with %d\n", error);
	    goto slsmm_dump_cleanup;
	}

	nanotime(&t_flush_disk);

	uprintf("suspend        %ldns\n", t_suspend_complete.tv_nsec-t_suspend.tv_nsec);
	uprintf("proc_ckpt  %ldns\n", t_proc_ckpt.tv_nsec-t_suspend_complete.tv_nsec);
	uprintf("fd_ckpt    %ldns\n", t_vmspace_ckpt.tv_nsec-t_proc_ckpt.tv_nsec);
	uprintf("vmspace        %ldns\n", t_fd_ckpt.tv_nsec-t_vmspace_ckpt.tv_nsec);
	uprintf("total_suspend  %ldns\n", t_resumed.tv_nsec-t_suspend.tv_nsec);
	uprintf("flush_disk %ldns\n", t_flush_disk.tv_nsec-t_resumed.tv_nsec);

    slsmm_dump_cleanup:
	free(objects, M_SLSMM);
	free_dump(dump);

	return error;
}


static int
slsmm_restore(struct proc *p, struct sls_desc *descs, int ndescs)
{
	struct dump *dump;
	int error = 0;

	error = setup_hashtable();
	if (error != 0)
	    goto slsmm_restore_out;

	dump = compose_dump(descs, ndescs);
	if (!dump) {
	    return -1;
	}

	/*
	* Send the signal before locking, otherwise
	* the thread state flags don't get updated.
	*
	* XXX We don't actually need that, right? We're overwriting ourselves,
	* so we definitely don't want to stop.
	*/
	kern_psignal(p, SIGSTOP);

	PROC_LOCK(p);

	/*
	* We unlink the thread in order to be able to reconstruct the process
	* thread state without it; if we keep it, when we go back to userspace
	* we pop the frame pushed in when syscalling. We could create a trampoline
	* or sth, but this is simpler.
	*/
	thread_unlink(curthread);

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

	error = fd_restore(p->p_fd, &dump->filedesc);
	if (error != 0) {
	    printf("Error: fd_restore failed with error code %d\n", error);
	    goto slsmm_restore_out;
	}

	PROC_UNLOCK(p);
	kern_psignal(p, SIGCONT);


	free_dump(dump);
	cleanup_hashtable();

    slsmm_restore_out:

	return error;
}

static int
slsmm_ioctl(struct cdev *dev __unused, u_long cmd, caddr_t data,
	int flag __unused, struct thread *td)
{
	int error = 0;
	struct dump_param *dparam = NULL;
	struct restore_param *rparam = NULL;
	struct proc *p = NULL;
	struct sls_desc *descs = NULL;
	struct sls_desc desc;
	int *fds = NULL;
	int fd_type;
	int ndescs;
	int i;

	/*
	* Get a hold of the process to be
	* checkpointed, bringing its thread stack to memory
	* and preventing it from exiting, to avoid race
	* conditions.
	*/
	/*
	* XXX Is holding appropriate here? Is it done correctly?
	* There _is_ a pfind() that just gets us the struct proc,
	* after all.
	*
	* This call brings the stack into memory,
	* but what about swapped out pages?
	*
	* In general, a nice optimization would be to use
	* swapped out pages as part of out checkpoint.
	*/
	if (cmd == SLSMM_RESTORE) {
	    rparam = (struct restore_param *) data;
	    fd_type = rparam->fd_type;
	    error = pget(curthread->td_proc->p_pid, PGET_WANTREAD, &p);

	} else {
	    dparam = (struct dump_param *) data;
	    fd_type = dparam->fd_type;
	    error = pget(dparam->pid, PGET_WANTREAD, &p);

	}
	if (error != 0) {
	    printf("Error: pget failed with code %d\n", error);
	    return error;
	}

	if (fd_type != SLSMM_FD_FILE && fd_type != SLSMM_FD_MEM) {
	    printf("Error: Invalid checkpoint fd type %d\n", fd_type);
	    error = EINVAL;
	    goto slsmm_ioctl_out;
	}

	switch (cmd) {
	    case FULL_DUMP:
		desc = create_desc(dparam->fd, dparam->fd_type, false);
		error = slsmm_dump(p, desc, SLSMM_CKPT_FULL);
		break;

	    case DELTA_DUMP:
		desc = create_desc(dparam->fd, dparam->fd_type, false);
		error = slsmm_dump(p, desc, SLSMM_CKPT_DELTA);
		break;

	    case SLSMM_RESTORE:
		fds = malloc(sizeof(int) * rparam->nfds, M_SLSMM, M_NOWAIT);
		if (fds == 0) {
		    printf("Error: Allocating fds failed\n");
		    error = ENOMEM;
		    goto slsmm_ioctl_out;
		}

		error = copyin(rparam->fds, fds, sizeof(int) * rparam->nfds);
		if (error != 0) {
		    printf("Error: Copying fds failed with %d \n", error);
		    goto slsmm_ioctl_out;
		}


		ndescs = rparam->nfds;
		descs = malloc(sizeof(struct sls_desc) * ndescs, M_SLSMM, M_NOWAIT);
		if (descs == NULL) {
		    printf("Error: Allocating descriptors failed\n");
		    error = ENOMEM;
		    goto slsmm_ioctl_out;
		}

		for (i = 0; i < ndescs; i++)
		    descs[i] = create_desc(fds[i], rparam->fd_type, true);

		error = slsmm_restore(p, descs, ndescs);
		break;
	}

    slsmm_ioctl_out:
	free(descs, M_SLSMM);
	free(fds, M_SLSMM);

	/* Release the hold we got when looking up the proc structure */
	PRELE(p);

	/* If the restore succeeded, this thread needs to die */
	if (cmd == SLSMM_RESTORE && error == 0) {
	    thread_link(td, p);
	    kern_thr_exit(curthread);
	}

	return error;
}

static struct cdevsw slsmm_cdevsw = {
    .d_version = D_VERSION,
    .d_ioctl = slsmm_ioctl,
};
static struct cdev *slsmm_dev;

static int
SLSMMHandler(struct module *inModule, int inEvent, void *inArg) {
	int error = 0;
	switch (inEvent) {
	    case MOD_LOAD:
		printf("Loaded\n");
		slsmm_dev = make_dev(&slsmm_cdevsw, 0, UID_ROOT, GID_WHEEL, 0666, "slsmm");
		md_init();
		break;
	    case MOD_UNLOAD:
		printf("Unloaded\n");
		md_clear();
		destroy_dev(slsmm_dev);
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
