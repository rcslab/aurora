#include <sys/param.h>
#include <sys/domain.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysent.h>
#include <sys/sysproto.h>

#include "sls_internal.h"
#include "sls_partition.h"
#include "sls_syscall.h"

/* The Aurora system call vector. */
struct sysentvec slssyscall_sysvec;

void
sls_exit_procremove(struct proc *p)
{
#ifdef INVARIANTS
	struct slspart *slsp;
	uint64_t oid = p->p_auroid;
#endif

	PROC_LOCK_ASSERT(p, MA_OWNED);

	/* Temporarily drop the process lock to prevent deadlocks. */
	PROC_UNLOCK(p);

#ifdef INVARIANTS

	KASSERT(oid != 0, ("Process not in Metropolis mode"));
	slsp = slsp_find(oid);
	KASSERT(slsp != NULL, ("Process in nonexistent partition %lu", oid));
	if (slsp == NULL) {
		PROC_LOCK(p);
		return;
	}

	slsp_deref(slsp);

#endif /* INVARIANTS*/

	SLS_LOCK();
	PROC_LOCK(p);

	LIST_REMOVE(p, p_aurlist);
	p->p_auroid = 0;

	cv_signal(&slsm.slsm_exitcv);
	SLS_UNLOCK();
}

/*
 * Replacement execve for all the processes in Aurora. Make sure the
 * function's custom syscall vector is not overwritten with the regular one.
 */
static int
slssyscall_execve(struct thread *td, void *data)
{
	struct execve_args *uap = (struct execve_args *)data;
	struct sysentvec *sysent_old;
	struct proc *p = td->td_proc;
	int error;

	sysent_old = p->p_sysent;
	error = sys_execve(td, uap);
	p->p_sysent = sysent_old;

	return (error);
}

static int
slssyscall_fork(struct thread *td, void *data)
{
	struct fork_args *uap = (struct fork_args *)data;
	struct proc *p;
	int error;
	pid_t pid;

	/* Don't race with an exiting module when adding the child to Aurora. */
	error = sls_startop();
	if (error != 0)
		return (error);

	error = sys_fork(td, uap);
	if (error != 0) {
		sls_finishop();
		return (error);
	}

	pid = td->td_retval[0];

	/* Try to find the new process and lock (it might have died already). */
	error = pget(pid, PGET_WANTREAD, &p);
	if (error != 0) {
		sls_finishop();
		return (0);
	}

	/* If the vector isn't the vanilla Aurora one, we we're in Metropolis.
	 */
	slsp_attach(curproc->p_auroid, p);

	/* If this process is in Metropolis so is the child. */
	p->p_sysent = curproc->p_sysent;

	PRELE(p);
	sls_finishop();

	return (0);
}

void
slssyscall_initsysvec(void)
{
	struct sysentvec *elf64_freebsd_sysvec;
	struct sysent *sls_sysent;
	struct proc *p = curproc;

	/*
	 * By copying everything over, we avoid needing to initialize the
	 * vector with INIT_SYSENTVEC which is impossible at this point since
	 * the macro is invoked at boot time.
	 */
	elf64_freebsd_sysvec = p->p_sysent;
	memcpy(&slssyscall_sysvec, elf64_freebsd_sysvec,
	    sizeof(slssyscall_sysvec));

	/* Fix up the actual system call table. */
	sls_sysent = malloc(
	    sizeof(*sls_sysent) * slssyscall_sysvec.sv_size, M_SLSMM, M_WAITOK);

	memcpy(sls_sysent, elf64_freebsd_sysvec->sv_table,
	    sizeof(*sls_sysent) * slssyscall_sysvec.sv_size);

	sls_sysent[SYS_execve].sy_call = &slssyscall_execve;
	sls_sysent[SYS_fork].sy_call = &slssyscall_fork;

	slssyscall_sysvec.sv_table = sls_sysent;
}

void
slssyscall_finisysvec(void)
{
	free(slssyscall_sysvec.sv_table, M_SLSMM);
}
