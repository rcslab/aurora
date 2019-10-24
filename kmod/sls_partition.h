#ifndef _slspart_H_
#define _slspart_H_

#include <sys/param.h>


#include <sys/sbuf.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include <machine/pmap.h>

#include <sls_ioctl.h>

#include "sls_kv.h"

/* Possible states of an slspart */
#define	SPROC_AVAILABLE	    0	/* Process is not doing anything */
#define	SPROC_CHECKPOINTING 1	/* Process is currently being checkpointed */
#define SPROC_DETACHED	    2	/* Process has been detached */

struct slspart {
    /* XXX Change from PIDs to lists of PIDs */
    uint64_t		    slsp_pid;	    /* PID of proc */
    uint64_t		    slsp_epoch;	    /* Current epoch, incremented after ckpt */

    int			    slsp_status;    /* Status of checkpoint */
    struct sls_attr	    slsp_attr;	    /* Parameters for checkpointing the process */
    int			    slsp_refcount;  /* Reference count for the process. */
    struct slskv_table	    *slsp_objects;  /* VM Objects created for the SLS */

    LIST_ENTRY(slspart) slsp_procs;	    /* List of checkpointed procs */
};

LIST_HEAD(slsp_list, slspart);
    
struct slspart *slsp_find(pid_t pid);
struct slspart *slsp_add(pid_t pid);
void slsp_del(pid_t pid);
void slsp_delall(void);
void slsp_fini(struct slspart *slsp);

void slsp_ref(struct slspart *slsp);
void slsp_deref(struct slspart *slsp);


#endif /* _slspart_H_ */
