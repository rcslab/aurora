#ifndef _SLS_PROCESS_H_
#define _SLS_PROCESS_H_

#include <sys/param.h>

#include <sls_snapshot.h>

struct sls_process {
    pid_t		    slsp_pid;
    struct mtx		    slsp_mtx;
    struct vmspace	    *slsp_vm;
    int			    slsp_ckptd;
    struct slss_list	    slsp_snaps;
    vm_ooffset_t	    slsp_charge;
    LIST_ENTRY(sls_process) slsp_procs;
};

LIST_HEAD(slsp_list, sls_process);

    
struct sls_process *slsp_add(pid_t pid);
void slsp_fini(struct sls_process *slsp);
void slsp_del(pid_t pid);
void slsp_delall(void);

struct sls_process *slsp_find(pid_t pid);

int slsp_add_snap(pid_t pid, struct sls_snapshot *slss);
int slsp_list_snap(pid_t pid);
/* XXX Need a "merge hashtables" function */
int slsp_list_compact(pid_t pid);

#endif /* _SLS_PROCESS_H_ */
