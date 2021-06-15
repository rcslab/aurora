#ifndef _SLS_H_
#define _SLS_H_

#include <sys/param.h>
#include <sys/condvar.h>
#include <sys/fcntl.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/sdt.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/uma.h>
#include <vm/vm_object.h>

#include <slos.h>
#include <slos_btree.h>
#include <slos_inode.h>
#include <slsfs.h>

#include "sls_kv.h"
#include "sls_partition.h"

SDT_PROVIDER_DECLARE(sls);

extern size_t sls_contig_limit;

struct sls_metadata {
	int slsm_exiting;		/* Is the SLS being destroyed? */
	struct slskv_table *slsm_parts; /* All (OID, slsp) pairs in the SLS */
	struct slskv_table *slsm_procs; /* All (PID, OID) pairs in the SLS */
	struct cdev *slsm_cdev;		/* The cdev that exposes the SLS ops */

	struct mtx slsm_mtx;   /* Structure mutex. */
	struct cv slsm_exitcv; /* CV for waiting on users to exit */
	int slsm_swapobjs;     /* Number of Aurora swap objects */
	int slsm_inprog;       /* Operations in progress */
	struct taskqueue *slsm_tabletq; /* Write taskqueue */
	LIST_HEAD(, proc) slsm_plist; /* List of processes in Aurora */
};

struct slsckpt_data {
	struct slskv_table *sckpt_rectable; /* In-memory records */
	struct slskv_table *sckpt_objtable; /* In-memory live VM Objects */
	slsset *sckpt_vntable;		    /* In-memory active vnodes */
	struct sls_attr sckpt_attr;	    /* Attributes of the partition */
#define sckpt_mode sckpt_attr.attr_mode
#define sckpt_target sckpt_attr.attr_target
};

/* An in-memory version of an Aurora record. */
struct sls_record {
	struct sbuf *srec_sb; /* The data of the record itself */
	uint64_t srec_id;     /* The unique ID of the record */
	uint64_t srec_type;   /* The record type */
};

/* The data needed to restore an SLS partition. */
struct slsrest_data {
	struct slskv_table
	    *objtable; /* Holds the new VM Objects, indexed by ID */
	struct slskv_table
	    *proctable; /* Holds the process records, indexed by ID */
	struct slskv_table
	    *oldvntable;	     /* The old vnode table that is not used */
	struct slskv_table *fptable; /* Holds the new files, indexed by ID */
	struct slskv_table
	    *kevtable; /* Holds the kevents for a kq, indexed by kq */
	struct slskv_table
	    *pgidtable; /* Holds the old-new process group ID pairs */
	struct slskv_table *sesstable; /* Holds the old-new session ID pairs */
	struct slskv_table *mbuftable; /* Holds the mbufs used by processes */
	struct slskv_table *vntable; /* Holds all vnodes, indexed by vnode ID */
	struct slspart *slsp;	     /* The partition being restored */

	struct cv proccv;   /* Used for synchronization during restores */
	struct mtx procmtx; /* Used alongside the cv above */
	int proctds;	    /* Same, used to create a restore time barrier */
	struct slsmetr slsmetr; /* Metropolis state */
};

extern struct sls_metadata slsm;

struct sls_record *sls_getrecord(
    struct sbuf *sb, uint64_t slsid, uint64_t type);
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
#define SLS_DBG(fmt, ...)                                     \
	do {                                                  \
		printf("(%s: Line %d) ", __FILE__, __LINE__); \
		printf(fmt, ##__VA_ARGS__);                   \
	} while (0)

#else

#define SLS_DBG(fmt, ...)

#define sls_tmp(fmt, ...) panic("debug printf not removed")
#endif /* SLS_MSG */

struct sls_checkpointd_args {
	struct slspart *slsp;
	struct proc *pcaller;
	bool recurse;
	uint64_t *nextepoch;
};

struct sls_restored_args {
	struct slspart *slsp;
	uint64_t daemon;
	uint64_t rest_stopped;
};

#define TONANO(tv) ((1000UL * 1000 * 1000 * (tv).tv_sec) + (tv).tv_nsec)
#define TOMICRO(tv) ((1000UL * 1000 * (tv).tv_sec) + (tv).tv_usec)

int slsckpt_dataregion(struct slspart *slsp, struct proc *p, vm_ooffset_t addr,
    uint64_t *nextepoch);
void sls_checkpointd(struct sls_checkpointd_args *args);
void sls_restored(struct sls_restored_args *args);

void slsrest_kqattach_locked(struct proc *p, struct kqueue *kq);
void slsrest_kqattach(struct proc *p, struct kqueue *kq);
void slsrest_kqdetach(struct kqueue *kq);

/* The range in which OIDs can fall. */
#define SLS_OIDRANGE (64 * 1024)
#define SLS_OIDMIN (1)
#define SLS_OIDMAX ((SLS_OIDMIN) + (SLS_OIDRANGE))

extern struct sysctl_ctx_list aurora_ctx;

/* Statistics and configuration variables accessible through sysctl. */
extern uint64_t sls_metadata_sent;
extern uint64_t sls_metadata_received;
extern uint64_t sls_data_sent;
extern uint64_t sls_data_received;
extern int sls_vfs_sync;
extern int sls_drop_io;
extern uint64_t sls_pages_grabbed;
extern uint64_t sls_io_initiated;
extern char *sls_basedir;

extern uint64_t sls_ckpt_attempted;
extern uint64_t sls_ckpt_done;
extern uint64_t sls_ckpt_duration;
SDT_PROVIDER_DECLARE(sls);

#define KTR_SLS KTR_SPARE4

#define SLS_ERROR(func, error)                                              \
	do {                                                                \
		printf("%s: %s in line %d (%s) failed with %d\n", __FILE__, \
		    #func, __LINE__, __func__, error);                      \
	} while (0)

#define SLS_ASSERT_LOCKED() (mtx_assert(&slsm.slsm_mtx, MA_OWNED))
#define SLS_ASSERT_UNLOCKED() (mtx_assert(&slsm.slsm_mtx, MA_NOTOWNED))
#define SLS_LOCK() mtx_lock(&slsm.slsm_mtx)
#define SLS_UNLOCK() mtx_unlock(&slsm.slsm_mtx)
#define SLS_EXITING() (slsm.slsm_exiting != 0)

/* Start an SLS ioctl operation. This guards global module state (not partition
 * state). */
static inline int
sls_startop(bool allow_concurrent)
{
	SLS_LOCK();
	if (SLS_EXITING() || (!allow_concurrent && (slsm.slsm_inprog > 0))) {
		SLS_UNLOCK();
		return (EBUSY);
	}

	/* Check if we're exiting. */
	if (slsm.slsm_exiting)
		return (EBUSY);

	slsm.slsm_inprog += 1;
	SLS_UNLOCK();
	return (0);
}

/* Mark an SLS ioctl operation as done. */
static inline void
sls_finishop(void)
{
	SLS_LOCK();
	slsm.slsm_inprog -= 1;
	cv_broadcast(&slsm.slsm_exitcv);
	SLS_UNLOCK();
}

/*
 * Reference the module, signaling it is in use.
 */
static inline int
sls_swapref(void)
{
	SLS_LOCK();
	if (SLS_EXITING() != 0) {
		SLS_UNLOCK();
		return (EBUSY);
	}
	slsm.slsm_swapobjs += 1;
	SLS_UNLOCK();

	return (0);
}

/*
 * Remove a reference to the module owned by a swap object.
 */
static inline void
sls_swapderef_unlocked(void)
{
	SLS_ASSERT_LOCKED();
	KASSERT(slsm.slsm_swapobjs > 0, ("module has no references left"));
	slsm.slsm_swapobjs -= 1;
	cv_broadcast(&slsm.slsm_exitcv);
}

static inline void
sls_swapderef(void)
{
	SLS_LOCK();
	sls_swapderef_unlocked();
	SLS_UNLOCK();
}

#define SLS_ZONEWARM (1024)
int sls_zonewarm(uma_zone_t zone);

int slsckpt_zoneinit(void);
void slsckpt_zonefini(void);

int slsrest_zoneinit(void);
void slsrest_zonefini(void);

int sls_checkpoint(struct sls_checkpoint_args *args);
int sls_attach(struct sls_attach_args *args);
int sls_restore(struct sls_restore_args *args);

void sls_procadd(uint64_t oid, struct proc *p, bool metropolis);
void sls_procremove(struct proc *p);

MALLOC_DECLARE(M_SLSMM);
MALLOC_DECLARE(M_SLSREC);

#endif /* _SLS_H_ */
