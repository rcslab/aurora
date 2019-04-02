#ifndef _SLS_H_
#define _SLS_H_

#include <vm/vm.h>
#include <vm/vm_object.h>

#include "sls_snapshot.h"
#include "sls_process.h"

/* XXX Decouple ioctl values from internal values */
#define SLS_CKPT_FULL	    0
#define SLS_CKPT_DELTA	    1


/* Benchmarking structures XXX improve/streamline somehow */
#define SLS_LOG_SLOTS	    9
#define SLS_LOG_ENTRIES	    10000
#define SLS_LOG_BUFFER	    1024

struct sls_metadata {
    long		slsm_log[SLS_LOG_SLOTS][SLS_LOG_ENTRIES];
    int			slsm_log_counter;
    int			slsm_exiting;
    int			slsm_lastid;
    u_long		slsm_procmask;
    struct slsp_list	*slsm_proctable;
    struct slss_list	slsm_snaplist;
    struct cdev		*slsm_cdev;
};

extern struct sls_metadata slsm;

inline long
tonano(struct timespec tp)
{
    const long billion = 1000UL * 1000 * 1000;

    return billion * tp.tv_sec + tp.tv_nsec;
}

inline void
sls_log(int type, int value)
{
    slsm.slsm_log[type][slsm.slsm_log_counter] = value;
}

inline int
sls_module_exiting(void)
{
    return slsm.slsm_exiting;
}

#endif /* _SLS_H_ */

