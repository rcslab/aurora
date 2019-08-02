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

#define SLS_CKPT_FULL	    0
#define SLS_CKPT_DELTA	    1


/* Benchmarking structures XXX improve/streamline somehow */
#define SLSLOG_PROC	    0
#define SLSLOG_MEM	    1
#define SLSLOG_FILE	    2
#define SLSLOG_FORK	    3
#define SLSLOG_COMPACT	    4
#define SLSLOG_CKPT	    5
#define SLSLOG_DUMP	    6
#define SLS_LOG_SLOTS	    7
#define SLS_LOG_ENTRIES	    (1024 * 128)
#define SLS_LOG_BUFFER	    1024


extern size_t sls_contig_limit;

struct sls_metadata {
    long		slsm_log[SLS_LOG_SLOTS][SLS_LOG_ENTRIES];
    int			slsm_log_counter;
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

inline void
sls_log(int type, int value)
{
    slsm.slsm_log[type][slsm.slsm_log_counter] = value;
}

inline void
sls_log_new(void)
{
    slsm.slsm_log_counter++;
}


inline int
sls_module_exiting(void)
{
    return slsm.slsm_exiting;
}


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


#endif /* _SLS_H_ */

