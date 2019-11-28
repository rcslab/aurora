#ifndef _SLS_H__
#define _SLS_H__

#include <sys/param.h>
#include <sys/mount.h>

#include "sls_ioctl.h"

/* Low-level APIs */
int sls_checkpoint(uint64_t oid, bool recurse);
int sls_restore(uint64_t oid, bool daemon);

int sls_attach(uint64_t oid, uint64_t pid);
int sls_partadd(uint64_t oid, const struct sls_attr attr);
int sls_partdel(uint64_t oid);

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
int sls_suspend(uint64_t oid);
int sls_resume(uint64_t oid);
int sls_getattr(uint64_t oid, struct sls_attr *attr);
int sls_setattr(uint64_t oid, const struct sls_attr *attr);
uint64_t sls_getckptid(uint64_t oid);

/* High-level APIs for Current Process */
bool sls_persistent();
int sls_ffork(int fd);
int sls_stat(int streamid, struct sls_stat *st);
int sls_barrier(int streamid);

#endif
