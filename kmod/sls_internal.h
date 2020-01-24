#ifndef _SLS_H_
#define _SLS_H_

#include <sys/param.h>
#include <sys/lock.h>

#include <sys/condvar.h>
#include <sys/fcntl.h>
#include <sys/mutex.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/vnode.h>

#include <vm/uma.h>
#include <vm/vm.h>
#include <vm/vm_object.h>

#include "sls_kv.h"
#include "sls_partition.h"

SDT_PROVIDER_DECLARE(sls);

extern size_t sls_contig_limit;

struct sls_metadata {
	int		    slsm_exiting;	/* Is the SLS being destroyed? */
	struct slskv_table  *slsm_parts;	/* All (OID, slsp) pairs in the SLS */
	struct slskv_table  *slsm_procs;	/* All (PID, OID) pairs in the SLS */ 
	struct cdev	    *slsm_cdev;		/* The cdev that exposes the SLS ops */

	/* OSD Related members */
	/* XXX Put the rectable and typetable in their own slsckpt_data structure */
	struct slskv_table  *slsm_rectable;	/* Associates in-memory pointers to data */
	struct slskv_table  *slsm_typetable;	/* Associates data with a record type */
	struct vnode	    *slsm_osdvp;	/* The device that holds the SLOS */

	int		    slsm_restoring;	/* Number of threads currently being restored */
	struct mtx	    slsm_mtx;		/* Structure mutex. */
	struct cv	    slsm_proccv;	/* Condition variable for when all processes are restored */
	struct cv	    slsm_donecv;	/* Condition variable to signal that processes can execute */
};

/* The data needed to restore an SLS partition. */
struct slsrest_data {
	struct slskv_table  *objtable;	/* Holds the new VM Objects, indexed by ID */
	struct slskv_table  *proctable;	/* Holds the process records, indexed by ID */
	struct slskv_table  *filetable;	/* Holds the new files, indexed by ID */
	struct slskv_table  *kevtable;	/* Holds the kevents for a kq, indexed by kq */
	struct slskv_table  *pgidtable;	/* Holds the old-new process group ID pairs */
	struct slskv_table  *sesstable;	/* Holds the old-new session ID pairs */
	struct slskv_table  *mbuftable;	/* Holds the mbufs used by processes */
	struct cv	    proccv;	/* Used as a barrier while creating pgroups */
	struct mtx	    procmtx;	/* Used alongside the cv above */
};

extern struct sls_metadata slsm;

#define FDTOFP(p, fd) (p->p_fd->fd_files->fdt_ofiles[fd].fde_file)

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
	struct slspart *slsp;
	uint64_t recurse;
};

struct sls_restored_args {
	uint64_t oid;
	uint64_t daemon;
};

void sls_checkpointd(struct sls_checkpointd_args *args);
void sls_restored(struct sls_restored_args *args);

#endif /* _SLS_H_ */

