#include "_slsmm.h"
#include "cpuckpt.h"
#include "dump.h"
#include "memckpt.h"
#include "slsmm.h"
#include "fileio.h"
#include "hash.h"

#include <sys/types.h>

#include <sys/conf.h>
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
slsmm_dump(struct proc *p, struct dump_param *param, int mode)
{
	int error = 0;
	struct thread_info *thread_infos = NULL;
	struct vm_map_entry_info *entries = NULL;
	struct dump *dump;
	vm_object_t *objects;
	vm_map_t vm_map = &p->p_vmspace->vm_map;
	struct timespec t_suspend, t_suspend_complete, t_proc_ckpt, t_vmspace, 
			t_resumed, t_flush_disk;

	/* 
	 * XXX Find a better way to do allocations, we cannot keep doing it 
	 * piecemeal. If we find a clever dump encoding, I predict it will be
	 * very elegant.
	 */

	dump = malloc(sizeof(struct dump), M_SLSMM, M_NOWAIT);
	if (!dump)
		return ENOMEM;

	thread_infos = malloc(sizeof(struct thread_info) * p->p_numthreads, 
			     M_SLSMM, M_NOWAIT);
	entries = malloc(sizeof(struct vm_map_entry_info) * vm_map->nentries, 
			M_SLSMM, M_NOWAIT);
	objects = malloc(sizeof(vm_object_t) * vm_map->nentries, 
			M_SLSMM, M_NOWAIT);

	dump->threads = thread_infos;
	dump->memory.entries = entries;

	if (!thread_infos || !objects || !entries) {
		error = ENOMEM;
		goto slsmm_dump_cleanup;
	}

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
	if (error) {
		printf("Error: proc_checkpoint failed with error code %d\n", error);
		goto slsmm_dump_cleanup;
	}
	nanotime(&t_proc_ckpt);


	/* Prepare the vmspace for dump */
	error = vmspace_checkpoint(p->p_vmspace, &dump->memory, objects, mode); 
	if (error) {
		printf("Error: vmspace_checkpoint failed with error code %d\n", error);
		goto slsmm_dump_cleanup;
	}
	nanotime(&t_vmspace);

	PROC_UNLOCK(p);

	/* Let the process execute ASAP */
	kern_psignal(p, SIGCONT);

	nanotime(&t_resumed);

	if (param->fd_type == SLSMM_FD_MEM) param->fd = new_md();

	error = fd_write(&dump->proc, sizeof(struct proc_info), param->fd, param->fd_type);
	if (error) {
		printf("Error: Writing process info dump failed with code %d\n", error);
		goto slsmm_dump_cleanup;
	}


	error = fd_write(thread_infos, sizeof(struct thread_info) * p->p_numthreads, param->fd, param->fd_type);
	if (error) {
		printf("Error: Writing thread info dump failed with code %d\n", error);
		goto slsmm_dump_cleanup;
	}

	error = vmspace_dump(dump, objects, param, mode);
	if (error) {
		printf("Error: vmspace_dump failed with error code %d\n", error);
		goto slsmm_dump_cleanup;
	}
	nanotime(&t_flush_disk);

	uprintf("suspend		%ldns\n", t_suspend_complete.tv_nsec-t_suspend.tv_nsec);
	uprintf("proc_ckpt	%ldns\n", t_proc_ckpt.tv_nsec-t_suspend_complete.tv_nsec);
	uprintf("vmspace		%ldns\n", t_vmspace.tv_nsec-t_proc_ckpt.tv_nsec);
	uprintf("total_suspend	%ldns\n", t_resumed.tv_nsec-t_suspend.tv_nsec); 
	uprintf("flush_disk	%ldns\n", t_flush_disk.tv_nsec-t_resumed.tv_nsec);

slsmm_dump_cleanup:
	free(objects, M_SLSMM);
	free_dump(dump);

	return error;
}


static int
slsmm_restore(struct proc *p, struct restore_param *param)
{
	printf("restore\n");
	struct dump *dump; 
	int error = 0;

	error = setup_hashtable();
	if (error)
		goto slsmm_restore_cleanup;

	dump = compose_dump(param);
	if (!dump)
		return -1;

	/* 
	 * Send the signal before locking, otherwise
	 * the thread state flags don't get updated.
	 */
	kern_psignal(p, SIGSTOP);

	PROC_LOCK(p);
	error = proc_restore(p, &dump->proc, dump->threads);
	if (error) {
		printf("Error: reg_restore failed with error code %d\n", error);
		goto slsmm_restore_cleanup;
	}

	error = vmspace_restore(p, &dump->memory);
	if (error) {
		printf("Error: vmspace_restore failed with error code %d\n", error);
		goto slsmm_restore_cleanup;
	}

	PROC_UNLOCK(p);
	kern_psignal(p, SIGCONT);


	free_dump(dump);
	cleanup_hashtable();
slsmm_restore_cleanup:

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
	 */
	if (cmd == SLSMM_RESTORE) {
		rparam = (struct restore_param *) data;
		error = pget(rparam->pid, PGET_WANTREAD, &p);

	} else {
		dparam = (struct dump_param *) data;
		error = pget(dparam->pid, PGET_WANTREAD, &p);

	}
	if (error) {
		printf("Error: pget failed with code %d\n", error);
		PRELE(p);
		return error;
	}

	/*
	 * TODO: Move this comment somewhere more appropriate
	 * In general, a nice optimization would be to use 
	 * swapped out pages as part of out checkpoint.
	 */
	switch (cmd) {
		case FULL_DUMP:
			error = slsmm_dump(p, dparam, SLSMM_CKPT_FULL);
			break;

		case DELTA_DUMP:
			error = slsmm_dump(p, dparam, SLSMM_CKPT_DELTA);
			break;

		case SLSMM_RESTORE:
			error = slsmm_restore(p, rparam);
			break;
	}

	/* Release the hold we got when looking up the proc structure */
	PRELE(p);

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
