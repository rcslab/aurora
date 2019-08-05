
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
sls_checkpoint(int pid)
{
	struct sls_checkpoint_args args;

	args.pid = pid;
	if (sls_ioctl(SLS_CHECKPOINT, &args) != 0) {
	    perror("sls_checkpoint");
	    return -1;
	}

	return 0;
}

/* Restore a process stored in sls_backend on top of the process with PID pid. */
int
sls_restore(int pid, struct sls_backend backend)
{
	struct sls_restore_args args;

	args.pid = pid;
	args.backend = backend;
	/* 
	 * We cannot copy an sbuf to the kernel as-is,
	 * so we have to expose the raw pointer.
	 */
	if (backend.bak_target == SLS_FILE) {
	    args.data = sbuf_data(backend.bak_name);
	    if (args.data == NULL)
		return -1;
	}

	if (sls_ioctl(SLS_RESTORE, &args) != 0) {
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
sls_attach(int pid, const struct sls_attr attr)
{
	struct sls_attach_args args;

	/* Setup the ioctl arguments. */
	args.pid = pid;
	args.attr = attr;
	/* 
	 * We cannot copy an sbuf to the kernel as-is,
	 * so we have to expose the raw pointer.
	 */
	if (attr.attr_backend.bak_target == SLS_FILE) {
	    args.data = sbuf_data(attr.attr_backend.bak_name);
	    if (args.data == NULL)
		return -1;
	}

	if (sls_ioctl(SLS_ATTACH, &args) != 0) {
	    perror("sls_attach");
	    return -1;
	}

	return 0;
}

/*
 * Detach a process from the SLS. If the process
 * is being checkpointed periodically, the 
 * checkpointing stops before detachment.
 */
int
sls_detach(int pid)
{
	struct sls_detach_args args;
	int ret;

	args.pid = pid;
	if (sls_ioctl(SLS_DETACH, &args) != 0) {
	    perror("sls_attach");
	    return -1;
	}

	return 0;
}

int
sls_suspend(int pid)
{
}

int
sls_resume(int pid)
{
}

int
sls_getattr(int pid, struct sls_attr *attr)
{
}

int
sls_setattr(int pid, const struct sls_attr *attr)
{
}

uint64_t
sls_getckptid(int pid)
{
}

int
sls_enter()
{
    /* XXX We can't enter without specifying a valid sls_attr */
}

int
sls_exit()
{
    return sls_detach(getpid());
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

