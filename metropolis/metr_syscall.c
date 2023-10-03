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

#include "../sls/sls_internal.h"
#include "../sls/sls_syscall.h"
#include "metr_internal.h"
#include "metr_syscall.h"

/* The Metropolis system call vector. */
struct sysentvec metrsys_sysvec;

static int
metrsys_getaddr(struct thread *td, int s, struct metr_listen *lsp)
{
	struct sockaddr_in *sa;
	socklen_t salen;
	int error;

	error = kern_getsockname(td, s, (struct sockaddr **)&sa, &salen);
	if (error != 0)
		return (error);

	KASSERT(salen == sizeof(*sa), ("unexpected salen %d", salen));
	memcpy(&lsp->metls_inaddr, &sa->sin_addr, sizeof(lsp->metls_inaddr));

	free(sa, M_SONAME);

	return (0);
}

static int
metrsys_register(struct thread *td, int s, struct sockaddr *sa,
    socklen_t *salen, int flags)
{
	struct proc *p = td->td_proc;
	uint64_t oid = p->p_auroid;
	struct metr_listen *lsp;
	int error;

	lsp = &metrm.metrm_listen[oid];

	/* State used to call accept() from the right invoked thread. */
	lsp->metls_proc = (uint64_t)p;
	lsp->metls_td = (uint64_t)curthread;
	lsp->metls_listfd = s;

	error = metrsys_getaddr(td, s, lsp);
	if (error != 0)
		return (error);

	/* State used to finish the accept() call after invocation. */
	lsp->metls_sa_user = (uintptr_t)sa;
	lsp->metls_salen_user = (uintptr_t)salen;
	lsp->metls_flags = flags;

	return (0);
}

static int
metrsys_checkpoint(struct thread *td, int s)
{
	struct sls_checkpoint_args checkpoint_args;
	uint64_t oid = td->td_proc->p_auroid;
	int error;

	/*
	 * Do not include the listening fd in the checkpoint.
	 * We will create a brand new socket at invocation time.
	 */
	error = kern_close(td, s);
	if (error != 0)
		METR_WARN("failed to close socket: %d\n", error);

	/*
	 * Trigger the checkpoint. Since we are in the call, the accept() call
	 * is not restarted after we're done (that would cause a loop, anyway).
	 */
	checkpoint_args = (struct sls_checkpoint_args) {
		.oid = oid,
		.recurse = true,
	};

	error = sls_checkpoint(&checkpoint_args);
	if (error != 0)
		return (error);

	/* Have the process exit. This also means exiting Metropolis mode. */
	exit1(td, 0, 0);
	panic("Process failed to exit");

	return (0);
}

static int
metrsys_accept4(struct thread *td, void *data)
{
	struct accept4_args *uap = (struct accept4_args *)data;
	int error;

	/* Set the partition's Metropolis process ID (not a pointer). */
	error = metrsys_register(td, uap->s, uap->name, uap->anamelen,
	    uap->flags);
	if (error != 0)
		return (error);

	return (metrsys_checkpoint(td, uap->s));
}

static int
metrsys_accept(struct thread *td, void *data __unused)
{
	struct accept_args *uap = (struct accept_args *)data;
	int error;

	/* Set the partition's Metropolis process ID (not a pointer). */
	error = metrsys_register(td, uap->s, uap->name, uap->anamelen,
	    ACCEPT4_INHERIT);
	if (error != 0)
		return (error);

	return (metrsys_checkpoint(td, uap->s));
}

/*
 * Overlay the accept() overloaded system calls functions on top of the regular
 * Aurora vector. The regular overloaded Aurora vector takes care of propagating
 * the P_METROPOLIS flag across fork() and exec() calls.
 */
void
metrsys_initsysvec(void)
{
	struct sysent *slssyscall_sysent;

	memcpy(&metrsys_sysvec, &slssyscall_sysvec, sizeof(metrsys_sysvec));

	slssyscall_sysent = malloc(sizeof(*slssyscall_sysent) *
		slssyscall_sysvec.sv_size,
	    M_METR, M_WAITOK);

	memcpy(slssyscall_sysent, slssyscall_sysvec.sv_table,
	    sizeof(*slssyscall_sysent) * slssyscall_sysvec.sv_size);

	slssyscall_sysent[SYS_accept].sy_call = &metrsys_accept;
	slssyscall_sysent[SYS_accept4].sy_call = &metrsys_accept4;

	metrsys_sysvec.sv_table = slssyscall_sysent;
}

void
metrsys_finisysvec(void)
{
	free(metrsys_sysvec.sv_table, M_METR);
}
