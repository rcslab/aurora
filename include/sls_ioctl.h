#ifndef _SLS_IOCTL_H_
#define _SLS_IOCTL_H_

#include <sys/ioccom.h>
#include <sys/sbuf.h>

/* The attributes of a process in the SLS. */
struct sls_attr {
	int		    attr_target;    /* Backend into which the process is checkpointed */
	int		    attr_mode;	    /* Full checkpoints or one of the delta modes? */
	int		    attr_period;    /* Checkpoint Period in ms */
	int		    attr_flags;	    /* Control flags */
};

struct sls_checkpoint_args {
	uint64_t	oid;		/* The OID of the partition to be checkpointed */
	bool		recurse;    	/* Include all descendants of attached processes */
	uint64_t	*nextepoch;	/* Epoch at which the checkpoint will be persistent */
};

struct sls_restore_args {
	uint64_t	    oid;	    /* OID of the partition being restored */
	uint64_t	    daemon;	    /* Restore the partition as a daemon */
	uint64_t	    rest_stopped;   /* Restored the partition in a stopped state */
};

struct sls_epochwait_args {
	uint64_t    oid;	    /* OID of partition */
	uint64_t    epoch;	    /* Epoch until which to wait */
	bool	    sync;	    /* Sleep if epoch not there yet? */
	bool	    *isdone;	    /* Is the epoch here? */
};

struct sls_attach_args {
	uint64_t	pid;	    /* PID of process being attached to the SLS */
	uint64_t	oid;	    /* OID of the partition in which we add the process */
};

struct sls_partadd_args {
	uint64_t	oid;	/* OID of the new partition. */
	struct sls_attr attr;	/* Checkpointing parameters for the process */
};

struct sls_partdel_args {
	uint64_t    oid;	/* OID of the partition to be detached from the SLS */
};

struct sls_memsnap_args {
	uint64_t	oid;		/* The OID of the partition to be checkpointed. */
	vm_ooffset_t	addr;		/* The address of the entry for checkpointing. */
	uint64_t	*nextepoch;	/* Epoch at which the checkpoint will be persistent */
};

#define SLS_CHECKPOINT		_IOW('d', 1, struct sls_checkpoint_args)
#define SLS_RESTORE		_IOW('d', 2, struct sls_restore_args)
#define SLS_ATTACH		_IOW('d', 3, struct sls_attach_args)
#define SLS_PARTADD		_IOW('d', 4, struct sls_partadd_args)
#define SLS_PARTDEL		_IOW('d', 5, struct sls_partdel_args)
#define SLS_EPOCHWAIT		_IOWR('d', 6, struct sls_epochwait_args)
#define SLS_MEMSNAP		_IOWR('d', 7, struct sls_memsnap_args)

#define SLS_DEFAULT_PARTITION	5115
#define SLS_DEFAULT_MPARTITION	5116

/*
 * XXX Use an encoding to catch errors like
 * using the wrong subcommand for a command.
 */

/*
 * Values for the mode field of the arguments for SLS_OP
 */
#define SLS_FULL		0   /* Full dump, all pages are saved */
#define SLS_DELTA		1   /* Delta dump, only modified pages are saved */
#define SLS_MODES		2   /* Number of modes */

/*
 * Values for the target field of the arguments for SLS_OP
 */
#define SLS_FILE	 	0   /* Input/output is a file */
#define SLS_MEM			1   /* Input/output is an in-memory dump */
#define SLS_OSD			2   /* Input/output is a single-level store */
#define SLS_TARGETS		3   /* Number of backends */

/* Control flags for partitions */
#define SLSATTR_IGNUNLINKED	0x1 /* Ignore unlinked files */
#define SLSATTR_LAZYREST	0x2 /* Restore lazily */

#define SLSATTR_FLAGISSET(attr, flag) (((attr).attr_flags & flag) != 0)
#define SLSATTR_ISIGNUNLINKED(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_IGNUNLINKED))
#define SLSATTR_ISLAZYREST(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_LAZYREST))

#endif
