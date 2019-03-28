#ifndef _SLS_OP_H_
#define _SLS_OP_H_

#include "sls_data.h"
#include "sls_proc.h"
#include "sls_mem.h"
#include "sls_fd.h"

#include "sls_dump.h"

struct sls_op_args {
    struct proc *p;
    int dump_mode;
    union {
	int id;
	char *filename;
    };
    int fd_type;
    int iterations;
    int interval;
};

void sls_checkpointd(struct sls_op_args *args);
void sls_restored(struct sls_op_args *args);
void sls_checkpoint_stop(struct proc *p);

#endif /* _SLS_OP_H_ */
