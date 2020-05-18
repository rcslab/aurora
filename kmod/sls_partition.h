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

/* Possible states of an slspart */
#define	SPROC_AVAILABLE	    0	/* Process is not doing anything */
#define	SPROC_CHECKPOINTING 1	/* Process is currently being checkpointed */
#define SPROC_DETACHED	    2	/* Process has been detached */

#define SPROC_EPOCHINIT	    1	/* Initial epoch for each partition */

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
    /* XXX slsp_mtx member */

    LIST_ENTRY(slspart)	    slsp_parts;	    /* List of active SLS partitions */
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
void slsp_epoch_advance(struct slspart *slsp);

#endif /* _SLSPART_H_ */
