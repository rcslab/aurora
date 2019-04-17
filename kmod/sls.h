#ifndef _SLS_H_
#define _SLS_H_

#include <sys/param.h>

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/vm_object.h>

#include "sls_snapshot.h"
#include "sls_process.h"
#include "sls_osd.h"

#define BITS_PER_BYTE 8

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

inline int
sls_module_exiting(void)
{
    return slsm.slsm_exiting;
}

/* XXX Ugly hack until we find out why VOP_WRITE is crashing on us */
extern int osdfd;
/* HACK */
inline void 
sls_osdhack()
{
	int error;
	char *osdname = "/dev/stripe/st0";

	error = kern_openat(curthread, AT_FDCWD, osdname,
	    UIO_SYSSPACE, O_RDWR | O_DIRECT, S_IRWXU);
	if (error != 0) {
	    printf("Error: Hardcoded OSD %s could not be opened. Continuing.\n", osdname);

	}

	osdfd = curthread->td_retval[0];
}

#endif /* _SLS_H_ */

