#ifndef _SLS_PROCESS_H_
#define _SLS_PROCESS_H_

#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/vm_object.h>

#define HASH_MAX (4 * 1024)

struct dump_page {
	vm_ooffset_t vaddr;
	union {
	    vm_page_t page; 
	    void *data;
	};
	LIST_ENTRY(dump_page) next;
};

LIST_HEAD(page_list, dump_page);


struct sls_process {
    struct mtx	    slsp_mtx;
    char	    slsp_name[MAXCOMLEN + 1];
    struct page_list *slsp_pages;
    u_long	    slsp_hashmask;
    int		    slsp_id;
//    struct proc	    *slsp_proc;
    struct dump	    *slsp_dump;
    TAILQ_ENTRY(sls_process) slsp_procs;
};

TAILQ_HEAD(slsp_tailq, sls_process);

extern struct slsp_tailq sls_procs;

struct sls_process *slsp_init(struct proc *p);
void slsp_fini(struct sls_process *slsp);

int slsp_init_htable(struct sls_process *slsp);
void slsp_fini_htable(struct sls_process *slsp);

void slsp_list(void);
void slsp_delete(int id);
struct sls_process *slsp_find(int id);

void slsp_addpage_noreplace(struct sls_process *slsp, struct dump_page *dump_page);
void slsp_addpage_replace(struct sls_process *slsp, struct dump_page *dump_page);
#endif /* _SLS_PROCESS_H_ */

