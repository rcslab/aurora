#ifndef _SLS_IOCTL_H_
#define _SLS_IOCTL_H_

#include <sys/ioccom.h>

/*
 * SLS Operation
 *
 * A checkpoint or restore operation.
 */
struct op_param {
	int	    op;		    /* Operation code (checkpoint, restore, etc.) */
	int	    pid;	    /* PID of process to be checkpointed/restored */
	int	    target;	    /* Type of input/output (filename, PID)*/
	void    *name;		    /* Filename */
	union {			    /* Either a filename or a dump ID*/
	    int	    id;		    /* Dump ID */
	    size_t  len;	    /* Filename length */
	};
	int	    mode;	    /* Dump mode (full, delta) */
	int	    period;	    /* Period of periodic checkpoints in milliseconds */
	int	    iterations;	    /* Number of iterations for periodic checkpoints */
};

/* XXX Modify */
struct compose_param {
	int	nid;
	int	*id;
};

/*
 * SLS In-Memory Dump Operation
 *
 * An operation that acts on the dumps that exist in memory.
 */
struct snap_param{
	int	    op;		    /* Operation code (list, delete, etc.) */
	int	    id;		    /* ID of dump (if applicable) */
};


#define SLS_OP			_IOWR('d', 1, struct op_param)
#define SLS_FLUSH_COUNT		_IOR('d', 3, int)
#define SLS_COMPOSE		_IOWR('d', 5, struct compose_param)
#define SLS_SNAP		_IOWR('d', 6, struct snap_param)

/* 
 * XXX Use an encoding to catch errors like 
 * using the wrong subcommand for a command.
 */

/*
 * Opcodes for SLS_OP. 
 */
#define SLS_CHECKPOINT		0   /* Checkpoint (One-off) */
#define SLS_RESTORE		1   /* Restore */
#define SLS_CKPT_STOP		2   /* Stop periodic checkpointing */

/*
 * Opcodes for SLS_SLSS
 */
#define SLS_SNAPLIST		0   /* List all active in-memory dumps */
#define SLS_SNAPDEL		1   /* Delete dump by ID if it exists */

/*
 * Values for the mode field of the arguments for SLS_OP
 */
#define SLS_FULL		0   /* Full dump, all pages are saved */
#define SLS_DELTA		1   /* Delta dump, only modified pages are saved */

/*
 * Values for the target field of the arguments for SLS_OP
 */
#define SLS_FILE	 	0   /* Input/output is a file */
#define SLS_MEM			1   /* Input/output is an in-memory dump */

#endif
