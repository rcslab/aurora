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
#include "sls_metropolis.h"
#include "sls_partition.h"

/* The Metropolis system call vector. */
static struct sysentvec slsmetr_sysvec;

/*
 * Called by the initial Metropolis function. Replaces the syscall vector with
 * one that has overloaded execve() and accept() calls. The accept() call is
 * really what we are after here, since it is used to demarcate the point in
 * which we checkpoint the function. We also overload execve(), which normally
 * override the system call vector, and fork(), so that new processes
 * dynamically enter themselves into the partition.
 *
 * The initial process entering Metropolis mode is NOT in Metropolis. This
 * allows us to use it to set up a new environment for each new Metropolis
 * instance.
 */
/* XXX For correctness we must also increment the module's use count, and use
 * hook exit() for Metropolis processes to decrement it. That way we avoid
 * removing the syster call vector from underneath running processes.
 */
int
sls_metropolis(struct sls_metropolis_args *args)
{
	struct proc *p = curthread->td_proc;

	/* Even though the process isn't in the SLS, it has an ID. */
	p->p_auroid = args->oid;
	p->p_sysent = &slsmetr_sysvec;

	return (0);
}

int
sls_metropolis_spawn(struct sls_metropolis_spawn_args *args)
{
	struct sls_restore_args rest_args;
	struct thread *td = curthread;
	struct slsmetr *slsmetr;
	struct slspart *slsp;
	int error;

	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (EINVAL);
	slsmetr = &slsp->slsp_metr;

	/*
	 * Get the new connected socket, save it in the partition. This call
	 * also fills in any information the restore process might need.
	 */
	error = kern_accept(td, args->s, &slsmetr->slsmetr_sa,
	    &slsmetr->slsmetr_namelen, &slsmetr->slsmetr_sockfp);
	slsp_deref(slsp);
	if (error != 0)
		return (error);

	rest_args = (struct sls_restore_args) {
		.oid = args->oid,
		.daemon = 0,
		.rest_stopped = 0,
	};

	/* Fully restore the partition. */
	return (sls_restore(&rest_args));
}

static int
slsmetr_setproc(uint64_t oid)
{
	struct proc *p = curproc;
	struct slspart *slsp;

	slsp = slsp_find(p->p_auroid);
	if (slsp == NULL)
		return (EINVAL);

	/* Save the SLS ID. */
	slsp->slsp_metr.slsmetr_proc = (uint64_t)p;
	slsp->slsp_metr.slsmetr_td = (uint64_t)curthread;

	slsp_deref(slsp);

	return (0);
}

static int
slsmetr_accept(struct thread *td, void *data)
{
	struct accept_args *uap = (struct accept_args *)data;
	struct sls_checkpoint_args checkpoint_args;
	struct proc *p = td->td_proc;
	int error;

	checkpoint_args = (struct sls_checkpoint_args) {
		.oid = p->p_auroid,
		.recurse = true,
	};

	/*
	 * Trigger the checkpoint. Since we are the call, the accept() call is
	 * not restarted after we're done (that would cause a loop, anyway).
	 */
	error = sls_checkpoint(&checkpoint_args);
	if (error != 0)
		return (error);

	/* Set the partition's assigned Metropolis process. */
	error = slsmetr_setproc(p->p_auroid);
	if (error != 0)
		return (error);

	/* Follow up with the actual accept() call. */
	return (sys_accept(td, uap));
}

/*
 * Replacement execve for all the processes in Metropolis. Make sure the
 * Metropolis syscall vector is not overwritten with the regular one.
 */
static int
slsmetr_execve(struct thread *td, void *data)
{
	struct execve_args *uap = (struct execve_args *)data;
	struct proc *p = td->td_proc;
	int error;

	error = sys_execve(td, uap);
	p->p_sysent = &slsmetr_sysvec;

	return (error);
}

/* Simple wrapper around fork(). */
static int
slsmetr_fork(struct thread *td, void *data)
{
	struct fork_args *uap = (struct fork_args *)data;
	struct proc *p = curthread->td_proc;
	struct sls_attach_args attach_args;
	int error;

	/* Simply wrap the regular fork call. */
	error = sys_fork(td, uap);
	if (error != 0)
		return (error);

	/*
	 * We have the PID of the new process, add it to the partition. We find
	 * the PID of our partition based on our PID.
	 */
	attach_args = (struct sls_attach_args) {
		.oid = p->p_auroid,
		.pid = td->td_retval[0],
	};

	/* Attach the new process into the partition. */
	error = sls_attach(&attach_args);
	if (error != 0)
		return (error);

	return (0);
}

void
sls_initsysvec(void)
{
	struct sysentvec *elf64_freebsd_sysvec;
	struct sysent *slsmetr_sysent;
	struct proc *p = curproc;

	/*
	 * By copying everything over, we avoid needing to initialize the
	 * vector with INIT_SYSENTVEC which is impossible at this point since
	 * the macro is invoked at boot time.
	 */
	elf64_freebsd_sysvec = p->p_sysent;
	memcpy(&slsmetr_sysvec, elf64_freebsd_sysvec, sizeof(slsmetr_sysvec));

	/* Fix up the actual system call table. */
	slsmetr_sysent = malloc(
	    sizeof(*slsmetr_sysent) * slsmetr_sysvec.sv_size, M_SLSMM,
	    M_WAITOK);

	memcpy(slsmetr_sysent, elf64_freebsd_sysvec->sv_table,
	    sizeof(*slsmetr_sysent) * slsmetr_sysvec.sv_size);

	slsmetr_sysent[SYS_fork].sy_call = &slsmetr_fork;
	slsmetr_sysent[SYS_execve].sy_call = &slsmetr_execve;
	slsmetr_sysent[SYS_accept].sy_call = &slsmetr_accept;

	slsmetr_sysvec.sv_table = slsmetr_sysent;
}

void
sls_finisysvec(void)
{
	free(slsmetr_sysvec.sv_table, M_SLSMM);
}
