#include "_slsmm.h"
#include "cpuckpt.h"
#include "memckpt.h"
#include "slsmm.h"
#include "fileio.h"

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
slsmm_dump(struct proc *p, int fd, int mode)
{
	int error = 0;
	int j = 0;
	struct proc_info *proc_info = NULL;
	struct thread_info *thread_infos = NULL;
	struct vm_map_entry_info *entries = NULL;
	vm_map_t vm_map = &p->p_vmspace->vm_map;
	vm_object_t *objects = NULL;
	struct timespec t_suspend, t_suspend_complete, t_proc_ckpt, t_vmspace, 
			t_resumed, t_flush_disk;

	proc_info = malloc(sizeof(struct proc_info), M_SLSMM, M_NOWAIT);
	proc_info->magic = SLS_PROC_INFO_MAGIC;
	thread_infos = malloc(sizeof(struct thread_info) * p->p_numthreads, 
			     M_SLSMM, M_NOWAIT);
	objects = malloc(sizeof(vm_object_t) * vm_map->nentries, M_SLSMM, M_NOWAIT);
	entries = malloc(sizeof(struct vm_map_entry_info) * vm_map->nentries, 
			M_SLSMM, M_NOWAIT);

	if (!proc_info || !thread_infos || !objects || !entries) {
		error = ENOMEM;
		goto slsmm_dump_cleanup;
	}

	// set magic number for proc and threads
	proc_info->magic = SLS_PROC_INFO_MAGIC;
	for (int i = 0; i < p->p_numthreads; i++)
		thread_infos[i].magic = SLS_THREAD_INFO_MAGIC;

	nanotime(&t_suspend);
	/* suspend the process */
	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	nanotime(&t_suspend_complete);

	/* Dump the process states */
	error = proc_checkpoint(p, proc_info, thread_infos);
	if (error) {
		printf("Error: proc_checkpoint failed with error code %d\n", error);
		goto slsmm_dump_cleanup;
	}
	nanotime(&t_proc_ckpt);


	/* prepare the vmspace for dump */
	error = vmspace_checkpoint(p->p_vmspace, objects, entries, mode); 
	if (error) {
		printf("Error: vmspace_checkpoint failed with error code %d\n", error);
		goto slsmm_dump_cleanup;
	}
	nanotime(&t_vmspace);

	/* Unlock the process ASAP to let it execute */
	kern_psignal(p, SIGCONT);
	PROC_UNLOCK(p);
	nanotime(&t_resumed);

	fd_write(proc_info, sizeof(struct proc_info), fd);
	fd_write(thread_infos, sizeof(struct thread_info) * p->p_numthreads, fd);

	error = vmspace_dump(p->p_vmspace, objects, entries, fd, mode);
	if (error) {
		printf("Error: vmspace_dump failed with error code %d\n", error);
		goto slsmm_dump_cleanup;
	}
	nanotime(&t_flush_disk);


	uprintf("j %d\n", j);
	uprintf("suspend %ldns\n", t_suspend_complete.tv_nsec-t_suspend.tv_nsec);
	uprintf("proc_ckpt	%ldns\n", t_proc_ckpt.tv_nsec-t_suspend_complete.tv_nsec);
	uprintf("vmspace		%ldns\n", t_vmspace.tv_nsec-t_proc_ckpt.tv_nsec);
	uprintf("total_suspend	%ldns\n", t_resumed.tv_nsec-t_suspend.tv_nsec); 
	uprintf("flush_disk	%ldns\n", t_flush_disk.tv_nsec-t_resumed.tv_nsec);

slsmm_dump_cleanup:
	if (objects) free(objects, M_SLSMM);
	if (entries) free(entries, M_SLSMM);
	if (proc_info) free(proc_info, M_SLSMM);
	if (thread_infos) free(thread_infos, M_SLSMM);

	return error;
}

static int
copy_proc_entries(struct dump *dst, struct dump *src)
{
	int error = 0;
	size_t size;

	dst->proc = src->proc;

	size = sizeof(struct thread_info) * src->proc.nthreads;
	dst->threads = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dst->threads) {
		printf("Error: cannot allocate thread_info\n");
		return ENOMEM;
	}
	memcpy(dst->threads, src->threads, size);

	dst->vmspace = src->vmspace;

	size = sizeof(struct vm_map_entry_info) * src->vmspace.nentries;
	dst->entries = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dst->entries) {
		printf("Error: cannot allocate entries\n");
		return ENOMEM;
	}
	memcpy(dst->entries, src->entries, size);

	size = sizeof(vm_object_t) * src->vmspace.nentries;
	dst->objects = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dst->objects) {
		printf("Error: cannot allocate object\n");
		return ENOMEM;
	}

	for (int i = 0; i < dst->vmspace.nentries; i++) {
		if (dst->entries[i].size == ULONG_MAX) {
			dst->objects[i] = NULL;
			continue;
		}
		dst->objects[i] = vm_object_allocate(OBJT_DEFAULT,
				dst->entries[i].size);
	}

	return error;
}

static int
copy_dump_pages(struct dump *dst, struct dump *src) 
{
	int error = 0;

	for (int i = 0; i < src->vmspace.nentries; i++) {
		vm_page_t page;
		vm_page_t dst_page;
		vm_offset_t vaddr, dst_vaddr;
		vm_object_t dst_object;

		if (src->objects[i] == NULL) continue;

		// find the corresponding vm_object in dst_dump
		dst_object = NULL;
		for (int j = 0; j < dst->vmspace.nentries; j ++)
			if (dst->entries[j].start == src->entries[i].start &&
			    dst->entries[j].offset == src->entries[i].offset) {
				dst_object = dst->objects[j];
				break;
			}
		if (!dst_object) continue;

		VM_OBJECT_WLOCK(dst_object);
		TAILQ_FOREACH(page, &src->objects[i]->memq, listq) if (page) {
			dst_page = vm_page_grab(dst_object, page->pindex, VM_ALLOC_NORMAL);
			if (dst_page == NULL) {
				printf("vm_page_alloc error\n");
				VM_OBJECT_WUNLOCK(dst_object);
				return ENOMEM;
			}

			vm_page_lock(page);
			vm_page_lock(dst_page);
			vaddr = pmap_map(NULL, page->phys_addr, page->phys_addr + PAGE_SIZE,
					VM_PROT_WRITE);
			dst_vaddr = pmap_map(NULL, dst_page->phys_addr, 
					dst_page->phys_addr + PAGE_SIZE, VM_PROT_WRITE);
			memcpy((void *)dst_vaddr, (void *)vaddr, PAGE_SIZE);
			vm_page_unlock(page);
			vm_page_unlock(dst_page);
			vm_page_xunbusy(dst_page);
		}
		VM_OBJECT_WUNLOCK(dst_object);
	}

	return error;
}

static int
slsmm_restore(struct proc *p, int nfds, int *fds)
{
	int error = 0;
	struct dump *dump = alloc_dump();
	struct dump *lastdump = alloc_dump();
	struct dump *currdump = alloc_dump();

	if (!dump || !lastdump || !currdump) {
		printf("Error: cannot allocate dump struct at slsmm_restore\n");
		goto slsmm_restore_cleanup;
	}

	error = load_dump(lastdump, fds[nfds-1]);
	if (error) {
		printf("Error: cannot load dumps\n");
		goto slsmm_restore_cleanup;
	}

	error = copy_proc_entries(dump, lastdump);

	for (int i = 0; i < nfds - 1; i++) {
		error = load_dump(currdump, fds[i]);
		copy_dump_pages(dump, currdump);
	}
	error = copy_dump_pages(dump, lastdump);
	if (error) {
		printf("Error: copy error\n");
		goto slsmm_restore_cleanup;
	}

	PROC_LOCK(p);
	kern_psignal(p, SIGSTOP);

	error = proc_restore(p, &dump->proc, dump->threads);
	if (error) {
		printf("Error: reg_restore failed with error code %d\n", error);
		goto slsmm_restore_cleanup;
	}

	error = vmspace_restore(p, dump);
	if (error) {
		printf("Error: vmspace_restore failed with error code %d\n", error);
		goto slsmm_restore_cleanup;
	}

	kern_psignal(p, SIGCONT);
	PROC_UNLOCK(p);

slsmm_restore_cleanup:
	free_dump(dump);
	free_dump(currdump);
	free_dump(lastdump);

	return error;
}

struct dump *alloc_dump() {
	struct dump *dump = NULL;
	dump = malloc(sizeof(struct dump), M_SLSMM, M_NOWAIT);
	if (dump == NULL) return NULL;
	dump->threads = NULL;
	dump->entries = NULL;
	dump->objects = NULL;
	return dump;
}

void free_dump(struct dump *dump) {
	if (!dump) return;
	if (dump->threads) free(dump->threads, M_SLSMM);
	if (dump->entries) free(dump->entries, M_SLSMM);
	if (dump->objects) free(dump->objects, M_SLSMM);
	free(dump, M_SLSMM);
}

int
load_dump(struct dump *dump, int fd)
{
	int error = 0;
	size_t size;

	// cleanup
	if (dump->threads) free(dump->threads, M_SLSMM);
	if (dump->entries) free(dump->entries, M_SLSMM);
	if (dump->objects) free(dump->objects, M_SLSMM);

	// load proc info
	error = fd_read(&dump->proc, sizeof(struct proc_info), fd);
	if (error) {
		printf("Error: cannot read proc_info\n");
		return error;
	}
	if (dump->proc.magic != SLS_PROC_INFO_MAGIC) {
		printf("Error: SLS_PROC_INFO_MAGIC not match\n");
		return -1;
	}

	// allocate thread info
	size = sizeof(struct thread_info) * dump->proc.nthreads;
	dump->threads = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dump->threads) {
		printf("Error: cannot allocate thread_info\n");
		return ENOMEM;
	}
	error = fd_read(dump->threads, size, fd);
	if (error) {
		printf("Error: cannot read thread_info\n");
		return error;
	}
	for (int i = 0; i < dump->proc.nthreads; i ++)
		if (dump->threads[i].magic != SLS_THREAD_INFO_MAGIC) {
			printf("Error: SLS_THREAD_INFO_MAGIC not match\n");
			return -1;
		}

	// load vmspace
	error = fd_read(&dump->vmspace, sizeof(struct vmspace_info), fd);
	if (error) {
		printf("Error: cannot read vmspace\n");
		return error;
	}
	if (dump->vmspace.magic != SLS_VMSPACE_INFO_MAGIC) {
		printf("Error: SLS_VMSPACE_INFO_MAGIC not match\n");
		return -1;
	}
	
	size = sizeof(struct vm_map_entry_info) * dump->vmspace.nentries;
	dump->entries = malloc(size, M_SLSMM, M_NOWAIT);
	size = sizeof(vm_object_t) * dump->vmspace.nentries;
	dump->objects = malloc(size, M_SLSMM, M_NOWAIT);
	if (!dump->entries || !dump->objects) {
		printf("Error: cannot allocate entries or objects\n");
		return ENOMEM;
	}

	//load entries and objects
	for (int i = 0; i < dump->vmspace.nentries; i++) {
		error = fd_read(dump->entries + i, sizeof(struct vm_map_entry_info), fd);
		if (error) 
			return error;
		if (dump->entries[i].magic != SLS_ENTRY_INFO_MAGIC) {
			printf("Error: SLS_ENTRY_INFO_MAGIC not match\n");
			return -1;
		}

		if (dump->entries[i].size == ULONG_MAX) {
			dump->objects[i] = NULL;
			continue;
		}

		dump->objects[i] = vm_object_allocate(OBJT_DEFAULT, 
				dump->entries[i].size);

		if (dump->objects[i] == NULL) {
			printf("vm_object_allocate error\n");
			return error;
		}

		VM_OBJECT_WLOCK(dump->objects[i]);
		for (;;) {
			vm_offset_t vaddr;
			vm_pindex_t poffset;
			vm_page_t page;

			error = fd_read(&poffset, sizeof(vm_pindex_t), fd);
			if (error) 
				return error;
			if (poffset == ULONG_MAX) break;

			page = vm_page_grab(dump->objects[i], poffset, VM_ALLOC_NORMAL);
			if (page == NULL) {
				printf("vm_page_alloc error\n");
				return ENOMEM;
			}

			vm_page_lock(page);
			vaddr = pmap_map(NULL, page->phys_addr, page->phys_addr + PAGE_SIZE,
					VM_PROT_WRITE);
			error = fd_read((void *)vaddr, PAGE_SIZE, fd);
			vm_page_unlock(page);

			if (error) {
				VM_OBJECT_WUNLOCK(dump->objects[i]);
				return error;
			}
		}
		VM_OBJECT_WUNLOCK(dump->objects[i]);
	}

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

	switch (cmd) {
		case FULL_DUMP:
			dparam = (struct dump_param *) data;
			if (error) {
				printf("Error: copyin failed with code %d\n", error);
				return error;
			}
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
			/*
			 * In general, a nice optimization would be to use 
			 * swapped out pages as part of out checkpoint.
			 */
			error = pget(dparam->pid, PGET_WANTREAD, &p);
			if (error) {
				printf("Error: pget failed with code %d\n", error);
				PRELE(p);
				return error;
			}

			error = slsmm_dump(p, dparam->fd, SLSMM_CKPT_FULL);
			break;

		case DELTA_DUMP:
			dparam = (struct dump_param *) data;
			if (error) {
				printf("Error: copyin failed with code %d\n", error);
				return error;
			}

			error = pget(dparam->pid, PGET_WANTREAD, &p);
			if (error) {
				printf("Error: pget failed with code %d\n", error);
				PRELE(p);
				return error;
			}

			slsmm_dump(p, dparam->fd, SLSMM_CKPT_DELTA);
			break;

		case SLSMM_RESTORE:
			rparam = (struct restore_param *) data;
			if (error) {
				printf("Error: copyin failed with code %d\n", error);
				return error;
			}
			error = pget(rparam->pid, PGET_WANTREAD, &p);
			if (error) {
				printf("Error: pget failed with code %d\n", error);
				PRELE(p);
				return error;
			}
			slsmm_restore(p, rparam->nfds, rparam->fds);
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
			break;
		case MOD_UNLOAD:
			printf("Unloaded\n");
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
