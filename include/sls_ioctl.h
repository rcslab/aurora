#ifndef _SLS_IOCTL_H_
#define _SLS_IOCTL_H_

#include <sys/ioccom.h>
#include <sys/sbuf.h>

/* An SLS backend for a process. */
struct sls_backend {
	int		bak_target; /* Type of backend */
	union {
	    struct sbuf	*bak_name;  /* Filename for file backends */
	    uint64_t	bak_id;	    /* Identifier for all other backends */
	};
};

/* The attributes of a process in the SLS. */
struct sls_attr {
	struct sls_backend  attr_backend;   /* Backend into which the process is checkpointed */
	int		    attr_mode;	    /* Full checkpoints or one of the delta modes? */
	int		    attr_period;    /* Checkpoint Period in ms */
};

struct sls_checkpoint_args {
	pid_t	    pid;	    /* The PID of the process to be checkpointed. */
};

struct sls_restore_args {
	pid_t		    pid;	/* PID of the process on top of which we restore */
	struct sls_backend  backend;	/* The backend where the SLS process is stored */
	void		    *data;	/* Miscellaneous data possibly needed */
};

struct proc_param {
	int	    op;		    /* Operation code (status, stop, etc.) */
	pid_t	    pid;	    /* PID of process */
	uint64_t    *ret;	    /* Output variable for the ioctl */
};

struct sls_attach_args {
	pid_t		pid;	/* PID of process being attached to the SLS */
	struct sls_attr attr;	/* Checkpointing parameters for the process */
	void		*data;	/* Miscellaneous data possibly needed */
};

struct sls_detach_args {
	pid_t pid;		/* PID of process being detached from the SLS */
};

#define SLS_CHECKPOINT		_IOWR('d', 1, struct sls_checkpoint_args)
#define SLS_RESTORE		_IOWR('d', 2, struct sls_restore_args)
#define SLS_ATTACH		_IOWR('d', 3, struct sls_attach_args)
#define SLS_DETACH		_IOWR('d', 4, struct sls_detach_args)
#define SLS_FLUSH_COUNT		_IOR('d', 5, int)
#define SLS_PROC		_IOWR('d', 6, struct proc_param)

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
#define SLS_MODES		2   /* Number of modes */

/*
 * Values for the target field of the arguments for SLS_OP
 */
#define SLS_FILE	 	0   /* Input/output is a file */
#define SLS_MEM			1   /* Input/output is an in-memory dump */
#define SLS_OSD			2   /* Input/output is a single-level store */
#define SLS_TARGETS		3   /* Number of backends */

#endif
