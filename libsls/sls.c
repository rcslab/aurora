
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

#include "sls_private.h"

/* 
 * Checkpoint a process already in the SLS. Processes being periodically checkpointed
 * cannot also get explicitly checkpointed, since the two operations would interfere
 * with each other. In order to change modes from periodic to explicit checkpointing,
 * a call to sls_set_attr() has been made to change the checkpointing period from 
 * zero to nonzero or vice versa.
 */
int
sls_checkpoint(uint64_t oid, bool recurse)
{
	struct sls_checkpoint_args args;

	args.oid= oid;
	args.recurse = recurse ? 1 : 0;

	if (sls_ioctl(SLS_CHECKPOINT, &args) < 0) {
	    perror("sls_checkpoint");
	    return -1;
	}

	return 0;
}

/* Restore a process stored in sls_backend on top of the process with PID pid. */
int
sls_restore(uint64_t oid, bool daemon)
{
	struct sls_restore_args args;

	args.oid = oid;
	args.daemon = daemon ? 1 : 0;

	if (sls_ioctl(SLS_RESTORE, &args) < 0) {
	    perror("sls_restore");
	    return -1;
	}

	return 0;
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
	    return -1;
	}

	return 0;
}

/*
 * Add an empty partition to the SLS.
 */
int
sls_partadd(uint64_t oid, const struct sls_attr attr)
{
	struct sls_partadd_args args;
	int ret;

	args.oid = oid;
	args.attr = attr;
	if (sls_ioctl(SLS_PARTADD, &args) != 0) {
	    perror("sls_attach");
	    return -1;
	}

	return 0;
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
	    perror("sls_attach");
	    return -1;
	}

	return 0;
}

int
sls_suspend(uint64_t oid)
{
}

int
sls_resume(uint64_t oid)
{
}

int
sls_getattr(uint64_t oid, struct sls_attr *attr)
{
}

int
sls_setattr(uint64_t oid, const struct sls_attr *attr)
{
}

uint64_t
sls_getckptid(uint64_t oid)
{
}


bool
sls_persistent()
{
    struct sls_attr attr;

    if (sls_getattr(getpid(), &attr) < 0)
	return false;
    else
	return true;
}

int
sls_ffork(int fd)
{

}
int
sls_stat(int streamid, struct sls_stat *st)
{
}

int
sls_barrier(int streamid)
{
    uint64_t ckptid = sls_getckptid(getpid());

    while (ckptid == sls_getckptid(getpid())) {
	usleep(1000);
    }

    return 0;
}

