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
#include "sls_syscall.h"

/* The Metropolis system call vector. */
struct sysentvec slsmetropolis_sysvec;

static int
slsmetropolis_set(uint64_t oid, struct file *fp, int flags)
{
	struct proc *p = curproc;
	struct slspart *slsp;

	slsp = slsp_find(p->p_auroid);
	if (slsp == NULL)
		return (EINVAL);

	/* Save the SLS ID. */
	slsp->slsp_metr.slsmetr_proc = (uint64_t)p;
	slsp->slsp_metr.slsmetr_td = (uint64_t)curthread;
	slsp->slsp_metr.slsmetr_flags = flags;
	slsp->slsp_metr.slsmetr_sockid = (uint64_t)fp->f_data;

	slsp_deref(slsp);

	return (0);
}

static int
slsmetropolis_attach(struct proc *p)
{
	struct sls_attach_args attach_args;

	/*
	 * We have the PID of the new process, add it to the partition. We find
	 * the PID of our partition based on our PID.
	 */
	attach_args = (struct sls_attach_args) {
		.oid = p->p_auroid,
		.pid = p->p_pid,
	};

	/* Attach the new process into the partition, and Aurora in general. */
	return (sls_attach(&attach_args));
}

static int
slsmetropolis_register(struct thread *td, int s, int flags)
{
	struct sls_checkpoint_args checkpoint_args;
	struct proc *p = td->td_proc;
	struct file *fp;
	int error;

	/* Avoid races with the slsmetropolis_fork() call that created us. */
	SLS_LOCK();
	if (SLS_EXITING()) {
		SLS_UNLOCK();
		return (EINTR);
	}

	/*
	 * No need to attach if already in the partition.
	 */
	if (!sls_proc_inpart(p->p_auroid, p)) {

		/*
		 * XXX Is there an easy way of finding
		 * out whether the process is already
		 * in a partition?
		 */

		/* We got our Aurora ID, no need to hold the lock anymore. */
		SLS_UNLOCK();
		/* XXX This is a race condition, what if someone
		 * tries to attach us after we unlock? */

		error = slsmetropolis_attach(p);
		if (error != 0)
			return (error);

	} else {

		/* Just unlock we're good to go. */
		SLS_UNLOCK();
	}

	/*
	 * Trigger the checkpoint. Since we are in the call, the accept() call
	 * is not restarted after we're done (that would cause a loop, anyway).
	 */
	checkpoint_args = (struct sls_checkpoint_args) {
		.oid = p->p_auroid,
		.recurse = true,
	};

	error = sls_checkpoint(&checkpoint_args);
	if (error != 0)
		return (error);

	/* Get the socket ID for the socket. */
	fp = FDTOFP(p, s);

	/* Set the partition's Metropolis process ID (not a pointer).  */
	error = slsmetropolis_set(p->p_auroid, fp, flags);
	if (error != 0)
		return (error);

	/* Have the process exit. This also means exiting Metropolis mode. */
	exit1(td, 0, 0);
	panic("Process failed to exit");

	return (0);
}

static int
slsmetropolis_accept4(struct thread *td, void *data)
{
	struct accept4_args *uap = (struct accept4_args *)data;

	return (slsmetropolis_register(td, uap->s, uap->flags));
}

static int
slsmetropolis_accept(struct thread *td, void *data __unused)
{
	struct accept_args *uap = (struct accept_args *)data;

	return (slsmetropolis_register(td, uap->s, ACCEPT4_INHERIT));
}

/*
 * Overlay the accept() overloaded system calls functions on top of the regular
 * Aurora vector. The regular overloaded Aurora vector takes care of propagating
 * the P_METROPOLIS flag across fork() and exec() calls.
 */
void
slsmetropolis_initsysvec(void)
{
	struct sysent *slssyscall_sysent;

	memcpy(&slsmetropolis_sysvec, &slssyscall_sysvec,
	    sizeof(slsmetropolis_sysvec));

	slssyscall_sysent = malloc(
	    sizeof(*slssyscall_sysent) * slssyscall_sysvec.sv_size, M_SLSMM,
	    M_WAITOK);

	memcpy(slssyscall_sysent, slssyscall_sysvec.sv_table,
	    sizeof(*slssyscall_sysent) * slssyscall_sysvec.sv_size);

	slssyscall_sysent[SYS_accept].sy_call = &slsmetropolis_accept;
	slssyscall_sysent[SYS_accept4].sy_call = &slsmetropolis_accept4;

	slsmetropolis_sysvec.sv_table = slssyscall_sysent;
}

void
slsmetropolis_finisysvec(void)
{
	free(slsmetropolis_sysvec.sv_table, M_SLSMM);
}
