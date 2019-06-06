#ifndef _SLS_OP_H_
#define _SLS_OP_H_

#include "sls_data.h"
#include "sls_proc.h"
#include "sls_mem.h"
#include "sls_fd.h"

#include "sls_dump.h"

struct sls_op_args {
    struct proc *p;
    int mode;
    char *filename;
    int id;
    int target;
    int iterations;
    int interval;
    struct sls_snapshot *slss;
    struct dump *dump;
};

void sls_ckptd(struct sls_op_args *args);
void sls_restd(struct sls_op_args *args);
void sls_ckpt_stop(struct proc *p);

#endif /* _SLS_OP_H_ */
