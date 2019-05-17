
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

#include "sls_private.h"

int
sls_attach(int pid, const struct sls_attr *attr)
{
}

int
sls_detach(int pid)
{
    int ret;
    struct proc_param param;

    param.op = SLS_PROCSTOP;
    param.pid = pid;
    param.ret = &ret;

    if (sls_proc(&param) < 0) {
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
    return sls_attach(getpid(), NULL);
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

