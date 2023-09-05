#ifndef _SLS_IOCTL_H_
#define _SLS_IOCTL_H_

#include <sys/ioccom.h>
#include <sys/sbuf.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The range in which OIDs can fall. */
#define SLS_OIDRANGE ((1 << 16) - 1)
#define SLS_OIDMIN (1)
#define SLS_OIDMAX ((SLS_OIDMIN) + (SLS_OIDRANGE))
#define SLOS_OBJOFF (64)

/* The attributes of a process in the SLS. */
struct sls_attr {
	int attr_target; /* Backend into which the process is checkpointed */
	int attr_mode;	 /* Full checkpoints or one of the delta modes? */
	int attr_period; /* Checkpoint Period in ms */
	int attr_flags;	 /* Control flags */
	size_t attr_amplification; /* Partition amplification factor */
};

struct sls_checkpoint_args {
	uint64_t oid; /* The OID of the partition to be checkpointed */
	bool recurse; /* Include all descendants of attached processes */
	uint64_t
	    *nextepoch; /* Epoch at which the checkpoint will be persistent */
};

struct sls_restore_args {
	uint64_t oid;	       /* OID of the partition being restored */
	uint64_t rest_stopped; /* Restored the partition in a stopped state */
};

struct sls_epochwait_args {
	uint64_t oid;	/* OID of partition */
	uint64_t epoch; /* Epoch until which to wait */
	bool sync;	/* Sleep if epoch not there yet? */
	bool *isdone;	/* Is the epoch here? */
};

struct sls_attach_args {
	uint64_t pid; /* PID of process being attached to the SLS */
	uint64_t oid; /* OID of the partition in which we add the process */
};

struct sls_partadd_args {
	uint64_t oid;	      /* OID of the new partition. */
	struct sls_attr attr; /* Checkpointing parameters for the process */
	int backendfd;	      /* File descriptor for the backend */
};

struct sls_partdel_args {
	uint64_t oid; /* OID of the partition to be detached from the SLS */
};

struct sls_memsnap_args {
	uint64_t oid;	   /* The OID of the partition to be checkpointed. */
	vm_ooffset_t addr; /* The address of the entry for checkpointing. */
	uint64_t
	    *nextepoch; /* Epoch at which the checkpoint will be persistent */
};

struct sls_metropolis_args {
	uint64_t oid; /* The OID of the partition for Metropolis mode */
};

struct sls_insls_args {
	uint64_t *oid; /* The OID of the partition, if in one */
	bool *insls;   /* Is the process in a partition? */
};

struct sls_metropolis_spawn_args {
	uint64_t oid; /* The OID of the Metropolis partition */
	int s;	      /* The socket to be connected */
};

struct sls_pgresident_args {
	uint64_t oid; /* The OID of the Metropolis partition */
	int fd;	      /* The file descriptor in which to dump the data */
};

#define SLS_CHECKPOINT _IOW('d', 1, struct sls_checkpoint_args)
#define SLS_RESTORE _IOW('d', 2, struct sls_restore_args)
#define SLS_ATTACH _IOW('d', 3, struct sls_attach_args)
#define SLS_PARTADD _IOW('d', 4, struct sls_partadd_args)
#define SLS_PARTDEL _IOW('d', 5, struct sls_partdel_args)
#define SLS_EPOCHWAIT _IOWR('d', 6, struct sls_epochwait_args)
#define SLS_MEMSNAP _IOWR('d', 7, struct sls_memsnap_args)
#define SLS_METROPOLIS _IOWR('d', 8, struct sls_metropolis_args)
#define SLS_INSLS _IOWR('d', 9, struct sls_insls_args)
#define SLS_METROPOLIS_SPAWN _IOWR('d', 10, struct sls_metropolis_spawn_args)
#define SLS_PGRESIDENT _IOWR('d', 11, struct sls_pgresident_args)

#define SLS_DEFAULT_PARTITION 5115
#define SLS_DEFAULT_MPARTITION 5116

/*
 * XXX Use an encoding to catch errors like
 * using the wrong subcommand for a command.
 */

/*
 * Values for the mode field of the arguments for SLS_OP
 */
#define SLS_FULL 0  /* Full dump, all pages are saved */
#define SLS_DELTA 1 /* Delta dump, only modified pages are saved */
#define SLS_MODES 2 /* Number of modes */

/*
 * Values for the target field of the arguments for SLS_OP
 */
#define SLS_FILE 0    /* Input/output is a file */
#define SLS_MEM 1     /* Input/output is an in-memory dump */
#define SLS_OSD 2     /* Input/output is a single-level store */
#define SLS_SOCKSND 3 /* Input is a socket to a remote server */
#define SLS_SOCKRCV 4 /* Output is a socket to a remote server */
#define SLS_TARGETS 5 /* Number of backends */

/* Control flags for partitions */
#define SLSATTR_IGNUNLINKED 0x1 /* Ignore unlinked files */
#define SLSATTR_LAZYREST 0x2	/* Restore lazily */
#define SLSATTR_CACHEREST 0x4	/* Cache restore images in memory */
#define SLSATTR_PREFAULT 0x8	/* Prefault popular pages */
#define SLSATTR_PRECOPY 0x10	/* Eagerly copy pages at restore time */
#define SLSATTR_DELTAREST 0x20	/* Delta restores */
#define SLSATTR_NOCKPT 0x40	/* Partition is not checkpointable */
#define SLSATTR_ASYNCSNAP 0x80	/* Do memsnap asynchronously */

#define SLSATTR_FLAGISSET(attr, flag) (((attr).attr_flags & flag) != 0)
#define SLSATTR_ISIGNUNLINKED(attr) \
	(SLSATTR_FLAGISSET((attr), SLSATTR_IGNUNLINKED))
#define SLSATTR_ISLAZYREST(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_LAZYREST))
#define SLSATTR_ISCACHEREST(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_CACHEREST))
#define SLSATTR_ISPREFAULT(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_PREFAULT))
#define SLSATTR_ISPRECOPY(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_PRECOPY))
#define SLSATTR_ISDELTAREST(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_DELTAREST))
#define SLSATTR_ISNOCKPT(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_NOCKPT))
#define SLSATTR_ISASYNCSNAP(attr) (SLSATTR_FLAGISSET((attr), SLSATTR_ASYNCSNAP))

#ifdef __cplusplus
}
#endif

#endif /* _SLS_IOCTL_H_ */
