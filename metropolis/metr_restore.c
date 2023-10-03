#include <sys/param.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/syscallsubr.h>
#include <sys/sysent.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include "../sls/sls_internal.h"
#include "metr_internal.h"

#define METROPOLIS_RETRIES (16)
#define METR_BACKLOG (512)

/*
 * XXXHACK If this is a listening socket for a Metropolis function,
 * assign it a random port, much like accept() does.
 *  Try randomly picking a number from 1024 to 65535
 *
 *  TODO: Every time accept() gets called from this special
 *  socket the Metropolis function goes into hibernation, waiting for
 *  a warm invocation. Metropolis cleans up unused functions after a
 *  timeout.
 */
static int
metr_listen(struct metr_listen *lsp)
{
	struct thread *td = curthread;
	struct sockaddr_in sa;
	int closeerr, error, i;
	struct socket *so;
	int fd;

	error = kern_socket(td, AF_INET, SOCK_STREAM, 0);
	if (error != 0)
		return (error);

	fd = td->td_retval[0];
	sa.sin_family = PF_INET;
	sa.sin_len = sizeof(sa);

	memcpy(&sa.sin_addr, &lsp->metls_inaddr, sizeof(sa.sin_addr));

	so = (struct socket *)FDTOFP(td->td_proc, fd)->f_data;
	so->so_options |= (SO_REUSEADDR | SO_REUSEPORT);

	for (i = 0; i < METROPOLIS_RETRIES; i++) {
		sa.sin_port = 1024 + (random() % (65545 - 1024));

		error = kern_bindat(td, AT_FDCWD, fd, (struct sockaddr *)&sa);

		switch (error) {
		/* If we connected we're done. */
		case 0:
			return (kern_listen(td, fd, METR_BACKLOG));

		/* If the address 4-tuple is taken, try again. */
		case EADDRINUSE:
		case EADDRNOTAVAIL:
			continue;

		/* Otherwise something went wrong. */
		default:
			closeerr = kern_close(td, fd);
			if (closeerr != 0)
				METR_WARN("failed to close socket: %d\n",
				    closeerr);

			return (error);
		}
	}

	/* Too many retries. */
	closeerr = kern_close(td, fd);
	if (closeerr != 0)
		METR_WARN("failed to close socket: %d\n", closeerr);

	return (EADDRINUSE);
}

static int
metr_connect(struct thread *td, struct metr_listen *lsp,
    struct metr_accept *acp)
{
	struct proc *p = td->td_proc;
	struct file *fp;
	int error;
	int fd;

	fp = acp->metac_fp;
	acp->metac_fp = NULL;

	FILEDESC_XLOCK(p->p_fd);
	/* Attach the file into the file table. */
	error = fdalloc(curthread, AT_FDCWD, &fd);
	if (error != 0) {
		fdrop(fp, curthread);
		FILEDESC_XUNLOCK(p->p_fd);
		return (error);
	}

	_finstall(p->p_fd, fp, fd, O_CLOEXEC, NULL);
	FILEDESC_XUNLOCK(p->p_fd);

	if (lsp->metls_sa_user != 0) {
		error = copyout(&acp->metac_in, (void *)lsp->metls_sa_user,
		    acp->metac_salen);
		if (error != 0)
			METR_WARN("copyout sockaddr %d\n", error);
	}

	if (lsp->metls_salen_user != 0) {
		error = copyout(&acp->metac_salen,
		    (void *)lsp->metls_salen_user, sizeof(acp->metac_salen));
		if (error != 0)
			METR_WARN("copyout socklen %d\n", error);
	}

	td->td_retval[0] = fd;
	(td->td_proc->p_sysent->sv_set_syscall_retval)(td, 0);

	return (0);
}

struct metr_slscb_args {
	struct metr_accept *acp;
	struct metr_listen *lsp;
};

static void
metr_slscb(struct proc *p, void *data)
{
	struct metr_slscb_args *args = (struct metr_slscb_args *)data;
	struct metr_listen *lsp = args->lsp;
	struct metr_accept *acp = args->acp;
	struct thread *td;
	int error;

	if (p->p_oldoid != lsp->metls_proc)
		return;

	error = metr_listen(lsp);
	if (error != 0)
		METR_WARN("Did not restore listening socket %d\n", error);

	FOREACH_THREAD_IN_PROC (p, td) {
		if (lsp->metls_td != td->td_oldtid)
			continue;

		error = metr_connect(td, lsp, acp);
		if (error != 0)
			METR_WARN("Did not attach accepted socket %d\n", error);

		break;
	}
}

int
metr_restore(uint64_t oid, struct metr_listen *lsp, struct metr_accept *acp)
{
	struct metr_slscb_args cb_args;
	struct slspart *slsp;
	int error;

	/* Get the partition to be checkpointed. */
	slsp = slsp_find(oid);
	if (slsp == NULL) {
		error = EINVAL;
		goto done;
	}

	/* Check if the partition is restorable. */
	if (!slsp_restorable(slsp)) {
		error = EINVAL;
		goto done;
	}

	cb_args = (struct metr_slscb_args) {
		.acp = acp,
		.lsp = lsp,
	};

	error = sls_rest(slsp, 0, metr_slscb, &cb_args);

done:
	if (slsp != NULL) {
		slsp_signal(slsp, error);
		slsp_deref(slsp);
	}

	return (error);
}
