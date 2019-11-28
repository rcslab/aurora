#ifndef _SLS_IOCTL_H_
#define _SLS_IOCTL_H_

#include <sys/ioccom.h>
#include <sys/sbuf.h>

/* The attributes of a process in the SLS. */
struct sls_attr {
	int		    attr_target;    /* Backend into which the process is checkpointed */
	int		    attr_mode;	    /* Full checkpoints or one of the delta modes? */
	int		    attr_period;    /* Checkpoint Period in ms */
};

struct sls_checkpoint_args {
	uint64_t	    oid;	/* The OID of the partition to be checkpointed. */
	uint64_t	    recurse;    /* Should we checkpoint all children of attached processes? */
};

struct sls_restore_args {
	uint64_t	    oid;    /* OID of the partition being restored */
	uint64_t	    daemon; /* Restore the partition as a daemon */
};

struct proc_param {
	int	    op;		    /* Operation code (status, stop, etc.) */
	pid_t	    pid;	    /* PID of process */
	uint64_t    *ret;	    /* Output variable for the ioctl */
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

#define SLS_CHECKPOINT		_IOWR('d', 1, struct sls_checkpoint_args)
#define SLS_RESTORE		_IOWR('d', 2, struct sls_restore_args)
#define SLS_ATTACH		_IOWR('d', 3, struct sls_attach_args)
#define SLS_PARTADD		_IOWR('d', 4, struct sls_partadd_args)
#define SLS_PARTDEL		_IOWR('d', 5, struct sls_partdel_args)

/* XXX These aren't well-defined, remove them and replace w/ sysctls */
#define SLS_FLUSH_COUNT		_IOR('d', 6, int)
#define SLS_PROC		_IOWR('d', 7, struct proc_param)

/* 
 * XXX Use an encoding to catch errors like 
 * using the wrong subcommand for a command.
 */

/*
 * Opcodes for SLS_SLSP
 */
#define SLS_PROCSTAT		0  /* Query status of process checkpointing */
#define SLS_PROCSTOP		1  /* Stop process checkpointing */

/*
 * Values for the mode field of the arguments for SLS_OP
 */
#define SLS_FULL		0   /* Full dump, all pages are saved */
#define SLS_DELTA		1   /* Delta dump, only modified pages are saved */
#define SLS_DEEP		2   /* Deep delta (explained in sls_ckpt.c) */
#define SLS_SHALLOW		3   /* Shallow delta (explained in sls_ckpt.c) */
#define SLS_MODES		4   /* Number of modes */

/*
 * Values for the target field of the arguments for SLS_OP
 */
#define SLS_FILE	 	0   /* Input/output is a file */
#define SLS_MEM			1   /* Input/output is an in-memory dump */
#define SLS_OSD			2   /* Input/output is a single-level store */
#define SLS_TARGETS		3   /* Number of backends */

#endif
