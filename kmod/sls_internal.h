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
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include <vm/uma.h>
#include <vm/vm.h>
#include <vm/vm_object.h>

#include <slos.h>
#include <slos_btree.h>
#include <slos_inode.h>
#include <slsfs.h>

#include "sls_kv.h"
#include "sls_mm.h"
#include "sls_partition.h"

SDT_PROVIDER_DECLARE(sls);

extern size_t sls_contig_limit;

struct sls_metadata {
    int		    slsm_exiting;	/* Is the SLS being destroyed? */
    struct slskv_table  *slsm_parts;	/* All (OID, slsp) pairs in the SLS */
    struct slskv_table  *slsm_procs;	/* All (PID, OID) pairs in the SLS */ 
    struct cdev	    *slsm_cdev;		/* The cdev that exposes the SLS ops */

    /* OSD Related members */
    struct vnode	    *slsm_osdvp;	/* The device that holds the SLOS */

    struct mtx	    slsm_mtx;		/* Structure mutex. */
    struct cv	    slsm_exitcv;	/* CV for waiting on users to exit. */
    int		    slsm_users;		/* Number of users of the module */
    /* XXX Move these outside, we can only restore one partition at a time.  */
    struct cv	    slsm_proccv;	/* Condition variable for when all processes are restored */
    struct cv	    slsm_donecv;	/* Condition variable to signal that processes can execute */
};

struct slsckpt_data {
    struct slskv_table *sckpt_rectable; /* In-memory records */
    struct slskv_table *sckpt_objtable; /* In-memory live VM Objects */
    struct sls_attr	sckpt_attr;	/* Attributes of the partition */
};

/* An in-memory version of an Aurora record. */
struct sls_record {
    struct sbuf *srec_sb;   /* The data of the record itself */
    uint64_t    srec_id;	/* The unique ID of the record */
    uint64_t    srec_type;	/* The record type */
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
    struct cv	    cv;		/* Global restore cv */
    struct mtx	    mtx;	/* Global restore mutex */
    int		    restoring;	/* Number of processes currently being restored */
};

extern struct sls_metadata slsm;

struct sls_record *sls_getrecord(struct sbuf *sb, uint64_t slsid, uint64_t type);
#define FDTOFP(p, fd) (p->p_fd->fd_files->fdt_ofiles[fd].fde_file)

/* The number of buckets for the hashtable used for the processes */
#define SLSP_BUCKETS (64)

/* Macros for turning in-object offsets to memory addresses and vice versa */
#define IDX_TO_VADDR(idx, entry_start, entry_offset) \
    (IDX_TO_OFF(idx) + entry_start - entry_offset)
#define VADDR_TO_IDX(vaddr, entry_start, entry_offset) \
    (OFF_TO_IDX(vaddr - entry_start + entry_offset))

/* Macros for debugging messages */
#ifdef SLS_MSG
#define SLS_DBG(fmt, ...) do {			    \
    printf("(%s: Line %d) ", __FILE__, __LINE__);   \
    printf(fmt, ##__VA_ARGS__);			    \
} while (0)

#else

#define SLS_DBG(fmt, ...)

#define sls_tmp(fmt, ...) panic("debug printf not removed")
#endif /* SLS_MSG */

struct sls_checkpointd_args {
    struct slspart *slsp;
    bool recurse;
    bool sync;
    struct mtx synch_mtx;
    struct cv synch_cv;
};

struct sls_restored_args {
    uint64_t oid;
    uint64_t daemon;
    uint64_t rest_stopped;
    int target;
};

#define TONANO(tv) ((1000UL * 1000 * 1000 * (tv).tv_sec) + (tv).tv_nsec)
#define TOMICRO(tv) ((1000UL * 1000 * (tv).tv_sec) + (tv).tv_usec)

void sls_checkpointd(struct sls_checkpointd_args *args);
void sls_restored(struct sls_restored_args *args);

int slsckpt_create(struct slsckpt_data **sckpt_datap, struct sls_attr *attr);
void slsckpt_destroy(struct slsckpt_data *sckpt_data, struct slsckpt_data *sckpt_new);

void slsrest_kqattach_locked(struct proc *p, struct kqueue *kq);
void slsrest_kqattach(struct proc *p, struct kqueue *kq);
void slsrest_kqdetach(struct kqueue *kq);

/* The range in which OIDs can fall. */
#define SLS_OIDRANGE	(64 * 1024)
#define SLS_OIDMIN 	(1)
#define SLS_OIDMAX  	((SLS_OIDMIN) + (SLS_OIDRANGE))

extern struct sysctl_ctx_list aurora_ctx;

/* Statistics and configuration variables accessible through sysctl. */
extern uint64_t sls_metadata_sent;
extern uint64_t sls_metadata_received;
extern uint64_t sls_data_sent;
extern uint64_t sls_data_received;
extern unsigned int sls_use_nulldev;
extern uint64_t sls_iochain_size;
extern struct file *sls_blackholefp;
extern int sls_sync;
extern int sls_drop_io;
extern uint64_t sls_pages_grabbed;
extern uint64_t sls_io_initiated;
extern char *sls_basedir;

extern uint64_t sls_ckpt_attempted;
extern uint64_t sls_ckpt_done;
SDT_PROVIDER_DECLARE(sls);

#define KTR_SLS KTR_SPARE4

#define SLS_ERROR(func, error)				    \
    do {						    \
	printf("%s: %s in line %d (%s) failed with %d\n",   \
	    __FILE__, #func, __LINE__, __func__, error);	    \
    } while (0)

/*
 * Reference the module, signaling it is in use.
 */
static inline int 
sls_modref(void)
{
	mtx_lock(&slsm.slsm_mtx);
	if  (slsm.slsm_exiting != 0) {
	    mtx_unlock(&slsm.slsm_mtx);
	    return (EINVAL);
	}

	slsm.slsm_users += 1;
	mtx_unlock(&slsm.slsm_mtx);

	return (0);
}

/*
 * Dererefence the module, possibly allowing it to unload.
 */
static inline void
sls_modderef(void)
{
	mtx_lock(&slsm.slsm_mtx);
	KASSERT(slsm.slsm_users > 0, ("module has no references left"));
	slsm.slsm_users -= 1;
	cv_broadcast(&slsm.slsm_exitcv);
	mtx_unlock(&slsm.slsm_mtx);

}

/* Global mutexes for restoring. */
extern struct mtx sls_restmtx;
extern struct cv sls_restcv;
extern int sls_resttds;

#endif /* _SLS_H_ */

