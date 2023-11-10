#ifndef _SLS_BACKEND_H_
#define _SLS_BACKEND_H_

#include <sys/param.h>

#define SLSBK_PRIVSIZE (4096)

struct slspart;
struct sls_backend;

struct sls_backend_ops {
	int (*slsbk_setup)(struct sls_backend *bk);
	int (*slsbk_teardown)(struct sls_backend *bk);
	int (*slsbk_import)(struct sls_backend *bk);
	int (*slsbk_export)(struct sls_backend *bk);
	int (*slsbk_partadd)(struct sls_backend *bk, struct slspart *slsp);
	int (*slsbk_setepoch)(struct sls_backend *bk, uint64_t oid,
	    uint64_t epoch);
};

struct sls_backend {
	int bk_type;
	struct sls_backend_ops *bk_ops;
	LIST_ENTRY(sls_backend) bk_backends;
	char bk_data[SLSBK_PRIVSIZE];
};

int slsbk_setup(struct sls_backend_ops *ops, int type,
    struct sls_backend **slsbkp);
int slsbk_teardown(struct sls_backend *slsbk);
int slsbk_import(struct sls_backend *slsbk);
int slsbk_export(struct sls_backend *slsbk);
int slsbk_partadd(struct sls_backend *slsbk, struct slspart *slsp);
int slsbk_setepoch(struct sls_backend *slsbk, uint64_t oid, uint64_t epoch);

extern struct sls_backend_ops slosbk_ops;
extern struct sls_backend_ops recvbk_ops;

#endif /* _SLS_BACKEND_H_ */
