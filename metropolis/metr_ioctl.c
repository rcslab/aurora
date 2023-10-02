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

#include "../sls/sls_metropolis.h"
#include "../sls/sls_partition.h"
#include "metr_internal.h"
#include "sls_ioctl.h"

struct metr_metadata metrm;

static int
metr_register(struct metr_register_args *args)
{
	struct proc *p = curthread->td_proc;

	/* Add the process in Aurora. */
	sls_procadd(args->oid, p, true);

	return (0);
}

/*
 * Create a new Metropolis process, handing it off a connected socket in the
 * process.
 */
static int
metr_invoke(struct metr_invoke_args *args)
{
	struct sls_restore_args rest_args;
	struct thread *td = curthread;
	struct slsmetr *slsmetr;
	struct slspart *slsp;
	int error;

	/*
	 * XXX Refactor this so that we do not grab the partition here.
	 * Also make sure that the socket fixup happens entirely outside
	 * of Aurora.
	 */

	slsp = slsp_find(args->oid);
	if (slsp == NULL)
		return (ENOENT);
	slsmetr = &slsp->slsp_metr;

	/*
	 * Get the new connected socket, save it in the partition. This call
	 * also fills in any information the restore process might need.
	 */
	error = kern_accept4(td, args->s, &slsmetr->slsmetr_sa,
	    &slsmetr->slsmetr_namelen, slsmetr->slsmetr_flags,
	    &slsmetr->slsmetr_sockfp);
	slsp_deref(slsp);
	if (error != 0)
		return (error);

	rest_args = (struct sls_restore_args) {
		.oid = args->oid,
		.rest_stopped = 0,
	};

	/* Fully restore the partition. */
	return (sls_restore(&rest_args));
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
	int error = 0;

	switch (inEvent) {
	case MOD_LOAD:
		bzero(&metrm, sizeof(metrm));

		mtx_init(&metrm.metrm_mtx, "metrm", NULL, MTX_DEF);
		cv_init(&metrm.metrm_exitcv, "metrm");

		/* Make the SLS available to userspace. */
		metrm.metrm_cdev = make_dev(&metr_cdevsw, 0, UID_ROOT,
		    GID_WHEEL, 0666, "metropolis");

		break;

	case MOD_UNLOAD:

		METR_LOCK();

		/*
		 * Destroy the device, wait for all ioctls in progress. We do
		 * this without the non-sleepable module lock.
		 */
		if (metrm.metrm_cdev != NULL)
			destroy_dev(metrm.metrm_cdev);

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
