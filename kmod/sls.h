#ifndef _SLS_H_
#define _SLS_H_

#include <sys/param.h>

#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/vnode.h>
#include <sys/sdt.h>

#include <vm/uma.h>
#include <vm/vm.h>
#include <vm/vm_object.h>

#include "sls_process.h"
#include "slskv.h"

SDT_PROVIDER_DECLARE(sls);

extern size_t sls_contig_limit;
extern struct slos slos;

struct sls_metadata {
    int			slsm_exiting;	/* Is the SLS being destroyed? */
    struct slskv_table	*slsm_proctable;    /* All processes in the SLS */
    struct cdev		*slsm_cdev;	/* The cdev that exposes the SLS' ops */

    /* OSD Related members */
    struct slskv_table	*slsm_rectable;	/* Associates in-memory pointers to data */
    struct slskv_table	*slsm_typetable;	/* Associates data with a record type */
    struct vnode	*slsm_osdvp;	/* The device that holds the SLOS */
    struct slsosd	*slsm_osd;	/* Similar to struct mount, but for the SLOS */
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

/* The number of buckets for the hashtable used for the processes */
#define SLSP_BUCKETS (64)

/* Macros for turning in-object offsets to memory addresses and vice versa */
#define IDX_TO_VADDR(idx, entry_start, entry_offset) \
	(IDX_TO_OFF(idx) + entry_start - entry_offset)
#define VADDR_TO_IDX(vaddr, entry_start, entry_offset) \
	(OFF_TO_IDX(vaddr - entry_start + entry_offset))

/* Macros for debugging messages */
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
	struct sls_backend backend;
};

struct sls_restored_args {
	struct proc *p;
	struct sls_backend backend;
};

void sls_checkpointd(struct sls_checkpointd_args *args);
void sls_restored(struct sls_restored_args *args);

#endif /* _SLS_H_ */

