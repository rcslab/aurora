
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sls_private.h"

/*
 * Checkpoint a partition with the given OID.
 */
int
sls_checkpoint(uint64_t oid, bool recurse)
{
	return (sls_checkpoint_epoch(oid, recurse, NULL));
}

/*
 * As above, but return the epoch at which the checkpoint will be done.
 */
int
sls_checkpoint_epoch(uint64_t oid, bool recurse, uint64_t *epoch)
{
	struct sls_checkpoint_args args;

	args.oid = oid;
	args.recurse = recurse ? 1 : 0;
	args.nextepoch = epoch;

	if (sls_ioctl(SLS_CHECKPOINT, &args) < 0) {
		perror("sls_checkpoint");
		return (-1);
	}

	return (0);
}

/* Restore a process stored in sls_backend on top of the process with PID pid.
 */
int
sls_restore(uint64_t oid, bool rest_stopped)
{
	struct sls_restore_args args;

	args.oid = oid;
	args.rest_stopped = rest_stopped ? 1 : 0;

	if (sls_ioctl(SLS_RESTORE, &args) < 0) {
		perror("sls_restore");
		return (-1);
	}

	return (0);
}

/*
 * Insert a process into the SLS. The process will start being checkpointed
 * periodically if the period argument is non-zero, otherwise it has to be
 * checkpointed via explicit calls to sls_checkpoint().
 */
int
sls_attach(uint64_t oid, uint64_t pid)
{
	struct sls_attach_args args;

	/* Setup the ioctl arguments. */
	args.oid = oid;
	args.pid = pid;

	if (sls_ioctl(SLS_ATTACH, &args) != 0) {
		perror("sls_attach");
		return (-1);
	}

	return (0);
}

/*
 * Add an empty partition to the SLS.
 */
int
sls_partadd(uint64_t oid, const struct sls_attr attr, int backendfd)
{
	struct sls_partadd_args args;
	int ret;

	args.oid = oid;
	args.attr = attr;
	args.backendfd = backendfd;

	if (sls_ioctl(SLS_PARTADD, &args) != 0) {
		perror("sls_partadd");
		return (-1);
	}

	return (0);
}

/*
 * Check if the provided epoch is here.
 */
int
sls_epochwait(uint64_t oid, uint64_t epoch, bool sync, bool *isdone)
{
	struct sls_epochwait_args args;
	int ret;

	if (sync && (isdone != NULL))
		return (EINVAL);

	if (!sync && (isdone == NULL))
		return (EINVAL);

	args.oid = oid;
	args.epoch = epoch;
	args.sync = sync;
	args.isdone = isdone;
	if (sls_ioctl(SLS_EPOCHWAIT, &args) != 0) {
		perror("sls_epoch");
		return (-1);
	}

	return (0);
}

/*
 * Sleep until the provided epoch is here.
 */
int
sls_untilepoch(uint64_t oid, uint64_t epoch)
{
	return (sls_epochwait(oid, epoch, true, NULL));
}

/*
 * Query whether the epoch is her
 */
int
sls_epochdone(uint64_t oid, uint64_t epoch, bool *isdone)
{
	return (sls_epochwait(oid, epoch, false, isdone));
}

/*
 * Detach a partition from the SLS. If the partition
 * is being checkpointed periodically, the
 * checkpointing stops before detachment.
 */
int
sls_partdel(uint64_t oid)
{
	struct sls_partdel_args args;
	int ret;

	args.oid = oid;
	if (sls_ioctl(SLS_PARTDEL, &args) != 0) {
		perror("sls_partdel");
		return (-1);
	}

	return (0);
}

/*
 * Checkpoint a single memory area into the SLS.
 */
int
sls_memsnap(uint64_t oid, void *addr)
{
	return (sls_memsnap_epoch(oid, addr, NULL));
}

/*
 * Same as above, also return the current epoch.
 */
int
sls_memsnap_epoch(uint64_t oid, void *addr, uint64_t *epoch)
{
	struct sls_memsnap_args args;
	int ret;

	args.oid = oid;
	args.addr = (vm_ooffset_t)addr;
	args.nextepoch = epoch;
	if (sls_ioctl(SLS_MEMSNAP, &args) != 0) {
		perror("sls_memsnap");
		return (-1);
	}

	return (0);
}

/*
 * Enter the function and all its subsequent children into the SLS as a
 * Metropolis function.
 */
int
sls_metropolis(uint64_t oid)
{
	struct metr_register_args args;

	args.oid = oid;

	if (metr_ioctl(METR_REGISTER, &args) < 0) {
		perror("metr_register");
		return (-1);
	}

	return (0);
}

/*
 * Enter the function and all its subsequent children into Metropolis.
 */
int
sls_insls(uint64_t *oid, bool *insls)
{
	struct sls_insls_args args;

	args.oid = oid;
	args.insls = insls;

	if (sls_ioctl(SLS_INSLS, &args) < 0) {
		perror("sls_insls");
		return (-1);
	}

	return (0);
}

/*
 * Create a new Metropolis function.
 */
int
sls_metropolis_spawn(uint64_t oid, int s)
{
	struct metr_invoke_args args;

	args.oid = oid;
	args.s = s;

	if (metr_ioctl(METR_INVOKE, &args) < 0) {
		perror("sls_metropolis_spawn");
		return (-1);
	}

	return (0);
}

/*
 * Create a new Metropolis function.
 */
int
sls_pgresident(uint64_t oid, int fd)
{
	struct sls_pgresident_args args;

	args.oid = oid;
	args.fd = fd;

	if (sls_ioctl(SLS_PGRESIDENT, &args) < 0) {
		perror("sls_pgresident");
		return (-1);
	}

	return (0);
}

int
sls_suspend(uint64_t oid)
{
	/*
	 * XXX For suspend, we need to be able to kill every single process in
	 * the partition. We can do that easily, just need to implement it. So
	 * it's a combination of checkpoint and a hypothetical remove partition
	 * operation.
	 */
	errno = ENOSYS;
	return (-1);
}

int
sls_resume(uint64_t oid)
{
	/*
	 * For resume, we need to recreate the partition and restore from it.
	 * So it's a combination of a hypothetical import and a restore.
	 */
	errno = ENOSYS;
	return (-1);
}

int
sls_getattr(uint64_t oid, struct sls_attr *attr)
{
	errno = ENOSYS;
	return (-1);
}

int
sls_ffork(int fd)
{
	errno = ENOSYS;
	return (-1);
}

int
sls_stat(int streamid, struct sls_stat *st)
{
	errno = ENOSYS;
	return (-1);
}
