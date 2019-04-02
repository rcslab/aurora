#ifndef _SLS_SNAPSHOT_H_
#define _SLS_SNAPSHOT_H_

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


struct sls_snapshot {
    struct mtx	    slss_mtx;			/* Mutex protecting the struct */
    pid_t	    slss_pid;			/* PID of the snap'd process */
    char	    slss_name[MAXCOMLEN + 1];	/* Name of the snap'd process */
    struct page_list *slss_pages;		/* Data pages of the snapshot */
    u_long	    slss_hashmask;		/* Hash mask for slss_pages */
    int		    slss_id;			/* Unique identifier for the snap */
    int		    slss_mode;			/* Type of snap (full, delta) */
    size_t	    slss_pagecount;		/* Size of data page hashtable */
    struct dump	    *slss_dump;			/* The dump of the snapshot */
    LIST_ENTRY(sls_snapshot) slss_procsnaps;	/* List of all snaps of a proc */
    LIST_ENTRY(sls_snapshot) slss_snaps;	/* Global snap list */
};

LIST_HEAD(slss_list, sls_snapshot);

struct sls_snapshot *slss_init(struct proc *p, int mode);
void slss_fini(struct sls_snapshot *slss);

int slss_init_htable(struct sls_snapshot *slss);
void slss_fini_htable(struct sls_snapshot *slss);

void slss_list(struct slss_list *slist);
void slss_listall(void);
void slss_delete(int id);
void slss_delete_all(void);
struct sls_snapshot *slss_find(int id);

void slss_addpage_noreplace(struct sls_snapshot *slss, struct dump_page *dump_page);
void slss_addpage_replace(struct sls_snapshot *slss, struct dump_page *dump_page);
#endif /* _SLS_SNAPSHOT_H_ */

