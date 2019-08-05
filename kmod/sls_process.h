#ifndef _SLS_PROCESS_H_
#define _SLS_PROCESS_H_

#include <sys/param.h>

#include <sys/sbuf.h>

#include <vm/vm.h>
#include <vm/vm_map.h>

#include <sls_ioctl.h>

/* Possible states of an SLS_PROCESS */
#define	SPROC_AVAILABLE	    0	/* Process is not doing anything */
#define	SPROC_CHECKPOINTING 1	/* Process is currently being checkpointed */
#define SPROC_DETACHED	    2	/* Process has been detached */

struct sls_process {
    uint64_t		    slsp_pid;	    /* PID of proc */
    uint64_t		    slsp_epoch;	    /* Current epoch, incremented after ckpt */

    struct vmspace	    *slsp_vm;	    /* vmspace created by last checkpoint */
    vm_ooffset_t	    slsp_charge;    /* Charge for the vmspace above */

    int			    slsp_status;    /* Status of checkpoint */
    struct sbuf		    *slsp_ckptbuf;  /* Buffer to be dumped at ckpt */
    struct sls_attr	    slsp_attr;	    /* Parameters for checkpointing the process */
    int			    slsp_refcount;  /* Reference count for the process. */

    LIST_ENTRY(sls_process) slsp_procs;	    /* List of checkpointed procs */
};

LIST_HEAD(slsp_list, sls_process);
    
struct sls_process *slsp_find(pid_t pid);
struct sls_process *slsp_add(pid_t pid);
void slsp_del(pid_t pid);
void slsp_delall(void);
void slsp_fini(struct sls_process *slsp);

void slsp_ref(struct sls_process *slsp);
void slsp_deref(struct sls_process *slsp);


#endif /* _SLS_PROCESS_H_ */
