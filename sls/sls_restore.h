#ifndef _SLS_RESTORE_H_
#define _SLS_RESTORE_H_

struct sls_restored_args {
	struct slspart *slsp;
	uint64_t daemon;
	uint64_t rest_stopped;
};

void sls_restored(struct sls_restored_args *args);

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

int slsrest_zoneinit(void);
void slsrest_zonefini(void);

#endif /* _SLS_RESTORE_H_ */
