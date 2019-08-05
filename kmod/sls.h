#ifndef _SLS_H_
#define _SLS_H_

#include <sys/param.h>

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/vnode.h>
#include <sys/sdt.h>

#include <vm/vm.h>
#include <vm/vm_object.h>

#include "sls_process.h"

SDT_PROVIDER_DECLARE(sls);

extern size_t sls_contig_limit;

struct sls_metadata {
    int			slsm_exiting;
    int			slsm_lastid;
    u_long		slsm_procmask;
    struct slsp_list	*slsm_proctable;
    struct cdev		*slsm_cdev;
    /* OSD Related members */
    struct vnode	*slsm_osdvp;
    struct slsosd	*slsm_osd;
    struct osd_mbmp	*slsm_mbmp;
};

extern struct sls_metadata slsm;

inline long
tonano(struct timespec tp)
{
    const long billion = 1000UL * 1000 * 1000;

    return billion * tp.tv_sec + tp.tv_nsec;
}

inline int
sls_module_exiting(void)
{
    return slsm.slsm_exiting;
}

#define SLS_DEBUG
#ifdef SLS_DEBUG
#define SLS_DBG(fmt, ...) do {			    \
    printf("(%s: Line %d) ", __FILE__, __LINE__);   \
    printf(fmt, ##__VA_ARGS__);			    \
    } while (0) 
#define sls_tmp(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define SLS_DBG(fmt, ...) 
#define sls_tmp(fmt, ...) panic("debug printf not removed")
#endif /* SLS_DEBUG */

struct sls_checkpointd_args {
	struct proc *p;
	struct sls_process *slsp;
};

struct sls_restored_args {
	struct proc *p;
	struct sbuf *filename;
	int target;
};

void sls_checkpointd(struct sls_checkpointd_args *args);
void sls_restored(struct sls_restored_args *args);

#endif /* _SLS_H_ */

