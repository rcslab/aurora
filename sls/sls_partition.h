#ifndef _SLSPART_H_
#define _SLSPART_H_

#include <sys/param.h>


#include <sys/sbuf.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include <machine/pmap.h>

#include <sls_ioctl.h>

#include "sls_kv.h"
#include "sls_internal.h"

#define SLSPART_EPOCHINIT   1	/* Initial epoch for each partition */

/* Possible states of an slspart */
#define	SLSP_AVAILABLE	    0	/* Partition not doing anything */
#define	SLSP_CHECKPOINTING  1	/* Partition is currently being checkpointed */
#define SLSP_DETACHED	    2	/* Partition has been detached */
#define SLSP_RESTORING	    3	/* Partition is being restored */

struct slspart {
    uint64_t		    slsp_oid;	    /* OID of the partition */
    uint64_t		    slsp_epoch;	    /* Current epoch, incremented after ckpt */

    slsset		    *slsp_procs;    /* The processes that belong to this partition */
    int			    slsp_status;    /* Status of checkpoint */
    struct sls_attr	    slsp_attr;	    /* Parameters for checkpointing the process */
    int			    slsp_refcount;  /* Reference count for the partition. */
    struct slskv_table	    *slsp_objects;  /* VM Objects created for the SLS */
    struct slsckpt_data	    *slsp_sckpt;    /* In-memory checkpoint */
    uint64_t		    slsp_procnum;   /* Number of processes in the partition */
    struct mtx		    slsp_syncmtx;   /* Mutex used for synchronization by the SLS */
    struct cv		    slsp_synccv;    /* CV used for synchronization by the SLS */
    int			    slsp_retval;    /* Return value of an operation done on the partition */
    bool		    slsp_syncdone;  /* Variable for slsp_signal/waitfor */

    LIST_ENTRY(slspart)	    slsp_parts;	    /* List of active SLS partitions */
#define slsp_target slsp_attr.attr_target
#define slsp_mode slsp_attr.attr_mode
};

LIST_HEAD(slsp_list, slspart);

struct slspart *slsp_find(uint64_t oid);

int slsp_attach(uint64_t oid, pid_t pid);
int slsp_detach(uint64_t oid, pid_t pid);

int slsp_add(uint64_t oid, struct sls_attr attr, struct slspart **slspp);
void slsp_del(uint64_t oid);

void slsp_delall(void);

void slsp_ref(struct slspart *slsp);
void slsp_deref(struct slspart *slsp);

int slsp_isempty(struct slspart *slsp);
void slsp_epoch_advance_major(struct slspart *slsp);
void slsp_epoch_advance_minor(struct slspart *slsp);

void slsp_signal(struct slspart *slsp, int retval);
int slsp_waitfor(struct slspart *slsp);

int slsp_setstate(struct slspart *slsp, int curstate, int nextstate, bool sleep);
int slsp_getstate(struct slspart *slsp);

#define SLSPART_IGNUNLINKED(slsp)  (SLSATTR_ISIGNUNLINKED((slsp)->slsp_attr))
#define SLSPART_LAZYREST(slsp)  (SLSATTR_ISLAZYREST((slsp)->slsp_attr))

#endif /* _SLSPART_H_ */
