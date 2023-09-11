#ifndef _SLSPART_H_
#define _SLSPART_H_

#include <sys/param.h>
#include <sys/sbuf.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/un.h>
#include <sys/unpcb.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#include <machine/pmap.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <sls_ioctl.h>

#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_metropolis.h"

#define SLSPART_EPOCHINIT 1 /* Initial epoch for each partition */

/* Possible states of an slspart */
#define SLSP_AVAILABLE 0     /* Partition not doing anything */
#define SLSP_CHECKPOINTING 1 /* Partition is currently being checkpointed */
#define SLSP_DETACHED 2	     /* Partition has been detached */
#define SLSP_RESTORING 3     /* Partition is being restored */

/* On-disk partition. */
struct slspart_serial {
	bool sspart_valid;
	uint64_t sspart_oid;
	struct sls_attr sspart_attr;
	uint64_t sspart_epoch;
	uint64_t sspart_proc;
	uint64_t sspart_td;
	int sspart_flags;
	uint64_t sspart_sockid;
};

/**/
struct slspart {
	uint64_t slsp_oid; /* OID of the partition */

	slsset *slsp_procs; /* The processes that belong to this partition */
	int slsp_status;    /* Status of checkpoint */
	struct sls_attr slsp_attr; /* Checkpointing paramaters */
	int slsp_refcount; /* Reference count for the partition. */

	struct slsckpt_data *slsp_sckpt;  /* In-memory checkpoint */
	uint64_t slsp_procnum; /* Number of processes in the partition */

	struct mtx slsp_syncmtx; /* Mutex used for synchronization by the SLS */
	struct cv slsp_synccv;	 /* CV used for synchronization by the SLS */
	bool slsp_syncdone;	 /* Variable for slsp_signal/waitfor */
	int slsp_retval;	 /* Return value of a partition operation */

	struct mtx slsp_epochmtx; /* Mutex used for guarding the epoch */
	struct cv slsp_epochcv;	  /* CV used for epoch events */
	uint64_t slsp_epoch;	  /* Current epoch, for ckpt on disk */
	uint64_t slsp_nextepoch; /* Epoch the caller's operations will be in
				    after completion*/
	void *slsp_backend; /* Opaque backend pointer, dependent on type */

	struct slsmetr slsp_metr; /* Local Metropolis state */

	LIST_ENTRY(slspart) slsp_parts; /* List of active SLS partitions */
	char slsp_name[PATH_MAX];	/* Path for the partition*/
	struct slsckpt_data *slsp_blanksckpt; /* Used for deltas */
#define slsp_target slsp_attr.attr_target
#define slsp_mode slsp_attr.attr_mode
#define slsp_amplification slsp_attr.attr_amplification
};

LIST_HEAD(slsp_list, slspart);

struct slspart *slsp_find_locked(uint64_t oid);
struct slspart *slsp_find(uint64_t oid);

int slsp_attach(uint64_t oid, struct proc *p, bool metropolis);
int slsp_detach(uint64_t oid, pid_t pid);

int slsp_add(
    uint64_t oid, struct sls_attr attr, int fd, struct slspart **slspp);
void slsp_del(uint64_t oid);

void slsp_delall(void);

void slsp_ref(struct slspart *slsp);
void slsp_deref(struct slspart *slsp);
void slsp_deref_locked(struct slspart *slsp);

int slsp_isempty(struct slspart *slsp);
uint64_t slsp_epoch_preadvance(struct slspart *slsp);
void slsp_epoch_advance(struct slspart *slsp, uint64_t next_epoch);

void slsp_signal(struct slspart *slsp, int retval);
int slsp_waitfor(struct slspart *slsp);

int slsp_setstate(
    struct slspart *slsp, int curstate, int nextstate, bool sleep);
int slsp_getstate(struct slspart *slsp);

bool slsp_hasproc(struct slspart *slsp, pid_t pid);
bool slsp_rest_from_mem(struct slspart *slsp);
bool slsp_restorable(struct slspart *slsp);

#define SLSP_IGNUNLINKED(slsp) (SLSATTR_ISIGNUNLINKED((slsp)->slsp_attr))
#define SLSP_LAZYREST(slsp) (SLSATTR_ISLAZYREST((slsp)->slsp_attr))
#define SLSP_CACHEREST(slsp) (SLSATTR_ISCACHEREST((slsp)->slsp_attr))
#define SLSP_PREFAULT(slsp) (SLSATTR_ISPREFAULT((slsp)->slsp_attr))
#define SLSP_PRECOPY(slsp) (SLSATTR_ISPRECOPY((slsp->slsp_attr)))
#define SLSP_DELTAREST(slsp) (SLSATTR_ISDELTAREST((slsp->slsp_attr)))
#define SLSP_NOCKPT(slsp) (SLSATTR_ISNOCKPT((slsp->slsp_attr)))

extern uma_zone_t slsckpt_zone;
int slsckpt_alloc(struct slspart *slsp, struct slsckpt_data **sckptp);
void slsckpt_clear(struct slsckpt_data *sckpt);
void slsckpt_hold(struct slsckpt_data *sckpt);
void slsckpt_drop(struct slsckpt_data *sckpt);
int slsckpt_addrecord(
    struct slsckpt_data *sckpt, uint64_t slsid, struct sbuf *sb, uint64_t type);

extern struct slspart_serial ssparts[];
int sslsp_deserialize(void);

static inline bool
slsp_isfullckpt(struct slspart *slsp)
{

	if (slsp->slsp_target == SLS_MEM)
		return (false);

	return (slsp->slsp_target == SLS_FULL);
}

static inline bool
slsp_proc_in_part(struct slspart *slsp, struct proc *p)
{
	struct slskv_iter iter;
	uint64_t pid;

	/* Make sure the process is in the partition. */
	KVSET_FOREACH(slsp->slsp_procs, iter, pid)
	{
		if (p->p_pid == (pid_t)pid)
			return (true);
	}

	return (false);
}

#endif /* _SLSPART_H_ */
