#ifndef _SLS_H__
#define _SLS_H__

#include <sys/param.h>
#include <sys/mount.h>

#include "sls_ioctl.h"

/* Low-level APIs */
int sls_checkpoint(int pid);
int sls_restore(int pid, struct sls_backend backend);

int sls_attach(int pid, const struct sls_attr attr);
int sls_detach(int pid);

int sls_proc(struct proc_param *param);

struct sls_stat {
    int type;
    int streamid;
    uint64_t ckptid;
};


/* Storage Control APIs */
int slos_openvol(const char *dev);
int slos_closevol(int backendid);
int slos_stat(struct statfs *sfs);

/* High-level APIs */
int sls_suspend(int pid);
int sls_resume(int pid);
int sls_getattr(int pid, struct sls_attr *attr);
int sls_setattr(int pid, const struct sls_attr *attr);
uint64_t sls_getckptid(int pid);

/* High-level APIs for Current Process */
int sls_enter();
int sls_exit();
bool sls_persistent();
int sls_ffork(int fd);
int sls_stat(int streamid, struct sls_stat *st);
int sls_barrier(int streamid);

#endif
