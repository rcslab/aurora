#ifndef _SLS_PROCESS_H_
#define _SLS_PROCESS_H_

#include <sys/param.h>

struct sls_process {
    uint64_t		    slsp_pid;	    /* PID of proc */
    uint64_t		    slsp_epoch;	    /* Current epoch, incremented after ckpt */

    struct vmspace	    *slsp_vm;	    /* vmspace created by last checkpoint */
    vm_ooffset_t	    slsp_charge;    /* Charge for the vmspace above */

    int			    slsp_status;    /* Status of checkpoint */

    LIST_ENTRY(sls_process) slsp_procs;	    /* List of checkpointed procs */
};

LIST_HEAD(slsp_list, sls_process);
    
struct sls_process *slsp_add(pid_t pid);
void slsp_fini(struct sls_process *slsp);
void slsp_del(pid_t pid);
void slsp_delall(void);

struct sls_process *slsp_find(pid_t pid);

#endif /* _SLS_PROCESS_H_ */
