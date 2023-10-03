#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bitstring.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/md5.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include "../sls/sls_partition.h"
#include "metr_internal.h"
#include "metr_syscall.h"
#include "sls_ioctl.h"

MALLOC_DEFINE(M_METR, "metropolis", "METR");

struct metr_metadata metrm;

static bool
metr_kill_always(struct proc *p)
{
	return (p->p_sysent == &metrsys_sysvec);
}

static int
metr_import()
{
	struct metr_listen *lsp;
	void *serial;
	int i;

	for (i = 0; i < METR_MAXLAMBDAS; i++) {
		KASSERT(sizeof(ssparts[i].sspart_private) >= sizeof(*lsp),
		    ("buffer too small"));

		lsp = &metrm.metrm_listen[i];
		serial = ssparts[i].sspart_private;
		memcpy(lsp, serial, sizeof(*lsp));
	}

	return (0);
}

static int
metr_export()
{
	struct metr_listen *lsp;
	void *serial;
	int i;

	/* We assume that METR_MAXLAMBDAS <= the number of total partitions. */
	for (i = 0; i < METR_MAXLAMBDAS; i++) {
		KASSERT(sizeof(ssparts[i].sspart_private) >= sizeof(*lsp),
		    ("buffer too small"));

		serial = ssparts[i].sspart_private;
		lsp = &metrm.metrm_listen[i];
		memcpy(lsp, serial, sizeof(*lsp));
	}

	return (0);
}

static int
metr_register(struct metr_register_args *args)
{
	struct proc *p = curthread->td_proc;

	/* Add the process into Aurora. */
	slsp_attach(args->oid, p);
	p->p_sysent = &metrsys_sysvec;

	return (0);
}

static int
metr_accept(struct thread *td, int s, struct metr_accept *acp, int flags)
{
	struct sockaddr_in *sa;
	int error;

	bzero(acp, sizeof(*acp));
	acp->metac_salen = sizeof(*sa);

	error = kern_accept4(td, s, (struct sockaddr **)&sa, &acp->metac_salen,
	    flags, &acp->metac_fp);
	if (error != 0)
		return (error);

	KASSERT(acp->metac_salen == sizeof(acp->metac_in),
	    ("salen %d", acp->metac_salen));
	memcpy(&acp->metac_in, sa, acp->metac_salen);

	free(sa, M_SONAME);

	return (0);
}

/*
 * Create a new Metropolis process, handing it off a connected socket in the
 * process.
 */
static int
metr_invoke(struct metr_invoke_args *args)
{
	struct thread *td = curthread;
	uint64_t oid = args->oid;
	struct metr_listen *lsp;
	struct metr_accept ac;
	int error;

	if (args->oid >= METR_MAXLAMBDAS)
		return (EINVAL);

	lsp = &metrm.metrm_listen[args->oid];
	if (lsp->metls_proc == 0)
		return (ENOENT);

	error = metr_accept(td, args->s, &ac, lsp->metls_flags);
	if (error != 0)
		return (error);

	error = metr_restore(oid, lsp, &ac);
	if (error != 0) {
		fdrop(ac.metac_fp, td);
		return (error);
	}

	return (0);
}

static int
metr_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int flag __unused,
    struct thread *td)
{
	int error = 0;

	if (sls_startop())
		return (EBUSY);

	switch (cmd) {

	case METR_REGISTER:
		error = metr_register((struct metr_register_args *)data);
		break;

	case METR_INVOKE:
		error = metr_invoke((struct metr_invoke_args *)data);
		break;

	default:
		error = EINVAL;
		break;
	}

	sls_finishop();

	return (error);
}

static struct cdevsw metr_cdevsw = {
	.d_version = D_VERSION,
	.d_ioctl = metr_ioctl,
};

static int
MetrHandler(struct module *inModule, int inEvent, void *inArg)
{
	struct cdev *cdev;
	int error = 0;

	switch (inEvent) {
	case MOD_LOAD:
		bzero(&metrm, sizeof(metrm));

		mtx_init(&metrm.metrm_mtx, "metrm", NULL, MTX_DEF);
		cv_init(&metrm.metrm_exitcv, "metrm");

		metrsys_initsysvec();

		error = metr_import();
		if (error != 0)
			METR_WARN("Could not import lambdas, error %d\n",
			    error);

		/* Make the SLS available to userspace. */
		metrm.metrm_cdev = make_dev(&metr_cdevsw, 0, UID_ROOT,
		    GID_WHEEL, 0666, "metropolis");

		break;

	case MOD_UNLOAD:

		METR_LOCK();
		cdev = metrm.metrm_cdev;
		metrm.metrm_cdev = NULL;
		METR_UNLOCK();

		if (cdev != NULL)
			destroy_dev(cdev);

		METR_LOCK();
		sls_kill(metr_kill_always);

		/* Add reference counting for live processes. */

		error = metr_export();
		if (error != 0)
			METR_WARN("Could not export lambdas, error %d\n",
			    error);

		metrsys_finisysvec();

		cv_destroy(&metrm.metrm_exitcv);
		mtx_destroy(&metrm.metrm_mtx);

		break;
	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static moduledata_t moduleData = { "metropolis", MetrHandler, NULL };

DECLARE_MODULE(metropolis, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_DEPEND(metropolis, sls, 0, 0, 0);
MODULE_VERSION(metropolis, 0);
