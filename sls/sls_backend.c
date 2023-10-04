#include <sys/param.h>
#include <sys/queue.h>

#include <sls_ioctl.h>

#include "sls_backend.h"
#include "sls_internal.h"
#include "sls_partition.h"

int
slsbk_setup(struct sls_backend_ops *ops, int type, struct sls_backend **slsbkp)
{
	struct sls_backend *slsbk;
	int error;

	slsbk = malloc(sizeof(*slsbk), M_SLSMM, M_NOWAIT | M_ZERO);
	if (slsbk == NULL)
		return (ENOMEM);

	slsbk->bk_type = type;
	slsbk->bk_ops = ops;

	error = (slsbk->bk_ops->slsbk_setup)(slsbk);
	if (error != 0) {
		free(slsbk, M_SLSMM);
		return (error);
	}

	*slsbkp = slsbk;

	return (0);
}

int
slsbk_teardown(struct sls_backend *slsbk)
{
	int error;

	error = (slsbk->bk_ops->slsbk_teardown)(slsbk);
	if (error != 0)
		return (error);

	free(slsbk, M_SLSMM);
	return (0);
}

int
slsbk_import(struct sls_backend *slsbk)
{
	return (slsbk->bk_ops->slsbk_import)(slsbk);
}

int
slsbk_export(struct sls_backend *slsbk)
{
	return (slsbk->bk_ops->slsbk_export)(slsbk);
}

int
slsbk_partadd(struct sls_backend *slsbk, struct slspart *slsp)
{
	slsp->slsp_bk = slsbk;
	return (slsbk->bk_ops->slsbk_partadd)(slsbk, slsp);
}

int
slsbk_setepoch(struct sls_backend *slsbk, uint64_t oid, uint64_t epoch)
{
	return (slsbk->bk_ops->slsbk_setepoch)(slsbk, oid, epoch);
}
