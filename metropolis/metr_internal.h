#ifndef _METR_INTERNAL_
#define _METR_INTERNAL_

#include <sys/param.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

struct metr_listen {
	uint64_t metls_proc; /* SLS ID of process to accept */
	uint64_t metls_td;   /* SLS ID of accepting thread */
	int metls_listfd;    /* File descriptor for the listening socket. */

	/* The listening socket state during accept() at checkpoint time. */
	struct in_addr metls_inaddr;

	/* The arguments passed to accept() at checkpoint time. */
	uintptr_t
	    metls_sa_user; /* Userspace pointer for the accept4() sa agument */
	uintptr_t metls_salen_user; /* Userspace pointer to the socket length */
	int metls_flags;	    /* flags for accept4() */
};

struct metr_accept {
	union {
		struct sockaddr_in in;
	} metac_sa;	       /* Address of the accepted socket. */
	socklen_t metac_salen; /* Size of the accepted address. */
	struct file *metac_fp;
};

#define metac_in metac_sa.in

#define METR_MAXLAMBDAS (65536)

struct metr_metadata {
	struct mtx metrm_mtx;	       /* Structure mutex. */
	struct cv metrm_exitcv;	       /* Structure CV. */
	LIST_HEAD(, proc) metrm_plist; /* List of Metropolis processes */
	uint64_t metrm_procs;	       /* Processes registered */
	struct cdev *metrm_cdev;       /* Metropolis API device */

	/* Maximum amount of registered lambdas. */
	struct metr_listen metrm_listen[METR_MAXLAMBDAS];
};

#define METR_ASSERT_LOCKED() (mtx_assert(&metrm.metrm_mtx, MA_OWNED))
#define METR_ASSERT_UNLOCKED() (mtx_assert(&metrm.metrm_mtx, MA_NOTOWNED))
#define METR_LOCK() mtx_lock(&metrm.metrm_mtx)
#define METR_UNLOCK() mtx_unlock(&metrm.metrm_mtx)
#define METR_EXITING() (metrm.metrm_exiting != 0)

#define METR_WARN(format, ...)                                              \
	do {                                                                \
		printf("(%s:%d) " format, __func__, __LINE__, __VA_ARGS__); \
	} while (0)

MALLOC_DECLARE(M_METR);

extern struct metr_metadata metrm;

int metr_restore(uint64_t oid, struct metr_listen *lsp,
    struct metr_accept *acp);

void metrsys_initsysvec(void);
void metrsys_finisysvec(void);

#endif /* _METR_INTERNAL_ */
