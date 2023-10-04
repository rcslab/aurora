#include <sys/param.h>

#include <sls_ioctl.h>

#include "debug.h"
#include "sls_backend.h"
#include "sls_internal.h"
#include "sls_io.h"

struct slspart_serial ssparts[SLS_OIDRANGE];

struct sls_backend_slos {
	int slosbk_type;
	struct sls_backend_ops *slosbk_ops;
	LIST_ENTRY(sls_backend) slosbk_backends;

	struct slos *slosbk_slos;
	bool slosbk_imported;
};

static int
slosbk_setup(struct sls_backend *slsbk)
{
	struct sls_backend_slos *slosbk = (struct sls_backend_slos *)slsbk;

	KASSERT(slosbk->slosbk_type == SLS_OSD, ("backend not a SLOS"));

	slosbk->slosbk_imported = false;
	slosbk->slosbk_slos = &slos;

	SLOS_LOCK(slosbk->slosbk_slos);
	if (slos_getstate(slosbk->slosbk_slos) != SLOS_MOUNTED) {
		SLOS_UNLOCK(slosbk->slosbk_slos);
		printf("No SLOS mount found.\n");
		return (EINVAL);
	}

	slos_setstate(slosbk->slosbk_slos, SLOS_WITHSLS);
	SLOS_UNLOCK(slosbk->slosbk_slos);

	return (0);
}

static int
slosbk_teardown(struct sls_backend *slsbk)
{
	struct sls_backend_slos *slosbk = (struct sls_backend_slos *)slsbk;
	struct slos *slos = slosbk->slosbk_slos;

	SLOS_LOCK(slos);
	/*
	 * The state might be not be SLOS_WITHSLS if we failed to
	 * load and are running this as cleanup.
	 */
	if (slos_getstate(slos) == SLOS_WITHSLS)
		slos_setstate(slos, SLOS_MOUNTED);
	SLOS_UNLOCK(slos);

	return (0);
}

/* Deserialize the already read on-disk partitions. */
static int
slosbk_deserialize(void)
{
	struct slspart *slsp;
	int error;
	int i;

	for (i = 0; i < SLS_OIDRANGE; i++) {
		if (!ssparts[i].sspart_valid)
			continue;

		/* Create the in-memory representation. */
		error = slsp_add(ssparts[i].sspart_oid, ssparts[i].sspart_attr,
		    -1, &slsp);
		if (error != 0)
			DEBUG1("Could not register partition %ld",
			    ssparts[i].sspart_oid);
	}

	return (0);
}

static int
slosbk_import(struct sls_backend *slsbk)
{
	struct sls_backend_slos *slosbk = (struct sls_backend_slos *)slsbk;
	size_t ssparts_len = sizeof(ssparts[0]) * SLS_OIDRANGE;
	struct thread *td = curthread;
	struct file *fp;
	int error;

	DEBUG1("[SSPART] Reading %ld bytes for partitions\n", ssparts_len);

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(SLOS_SLSPART_INODE, false, &fp);
	if (error != 0) {
		/*
		 * There were no partitions to speak of, because
		 * this is the first time we are mounting this SLOS.
		 */
		DEBUG("[SSPART] No partitions found\n");
		slosbk->slosbk_imported = true;
		return (0);
	}

	error = slsio_fpread(fp, ssparts, ssparts_len);
	if (error != 0) {
		fdrop(fp, td);
		return (error);
	}

	if (error == 0)
		slosbk->slosbk_imported = true;

	DEBUG("[SSPART] Done reading partitions\n");

	fdrop(fp, td);

	return (slosbk_deserialize());
}

static int
slosbk_export(struct sls_backend *slsbk)
{
	struct sls_backend_slos *slosbk = (struct sls_backend_slos *)slsbk;
	size_t ssparts_len = sizeof(ssparts[0]) * SLS_OIDRANGE;
	struct thread *td = curthread;
	struct file *fp;
	int error;

	if (!slosbk->slosbk_imported)
		return (0);

	/* Get the vnode for the record and open it. */
	error = slsio_open_sls(SLOS_SLSPART_INODE, true, &fp);
	if (error != 0)
		return (error);

	error = slsio_fpwrite(fp, ssparts, ssparts_len);
	DEBUG1("Wrote %ld bytes for partitions\n", ssparts_len);

	fdrop(fp, td);

	return (error);
}

static int
slosbk_partadd(struct sls_backend *slsbk, struct slspart *slsp)
{
	uint64_t oid = slsp->slsp_oid;

	ssparts[oid].sspart_valid = true;
	ssparts[oid].sspart_oid = slsp->slsp_oid;
	ssparts[oid].sspart_attr = slsp->slsp_attr;
	ssparts[oid].sspart_epoch = 0;

	return (0);
}

static int
slosbk_setepoch(struct sls_backend *slsbk, uint64_t oid, uint64_t epoch)
{
	ssparts[oid].sspart_epoch = epoch;
	return (0);
}

struct sls_backend_ops slosbk_ops = {
	.slsbk_setup = slosbk_setup,
	.slsbk_teardown = slosbk_teardown,
	.slsbk_import = slosbk_import,
	.slsbk_export = slosbk_export,
	.slsbk_partadd = slosbk_partadd,
	.slsbk_setepoch = slosbk_setepoch,
};
