#ifndef _SLS_H_
#define _SLS_H_

#include <sys/param.h>
#include <sys/mount.h>

#include <stdbool.h>

#include "sls_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Low-level APIs */
int sls_memsnap(uint64_t oid, void *addr);
int sls_memsnap_epoch(uint64_t oid, void *addr, uint64_t *epoch);
int sls_checkpoint(uint64_t oid, bool recurse);
int sls_checkpoint_epoch(uint64_t oid, bool recurse, uint64_t *epoch);
int sls_epochwait(uint64_t oid, uint64_t epoch, bool sync, bool *isdone);
int sls_restore(uint64_t oid, bool rest_stopped);

int sls_attach(uint64_t oid, uint64_t pid);
int sls_partadd(uint64_t oid, const struct sls_attr attr, int backendfd);
int sls_partdel(uint64_t oid);

struct sls_stat {
	int type;
	int streamid;
	uint64_t ckptid;
};

int sls_epochdone(uint64_t oid, uint64_t epoch, bool *isdone);
int sls_untilepoch(uint64_t oid, uint64_t epoch);
int sls_metropolis(uint64_t oid);
int sls_metropolis_spawn(uint64_t oid, int s);
int sls_insls(uint64_t *oid, bool *insls);

/* Storage Control APIs */
int slos_openvol(const char *dev);
int slos_closevol(int backendid);
int slos_stat(struct statfs *sfs);

/* High-level APIs */
int sls_suspend(uint64_t oid);
int sls_resume(uint64_t oid);
int sls_getattr(uint64_t oid, struct sls_attr *attr);

/* High-level APIs for Current Process */
int sls_ffork(int fd);
int sls_stat(int streamid, struct sls_stat *st);
int sls_barrier(int streamid);

/* Get list of resident pages in the partition. */
int sls_pgresident(uint64_t oid, int fd);

#ifdef __cplusplus
}
#endif

#endif /* _SLS_H_ */
