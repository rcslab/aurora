#ifndef _SLS_PRIVATE_H__
#define _SLS_PRIVATE_H__

#include "sls_ioctl.h"

/* Low-level APIs */
int sls_op(struct op_param *param);
int sls_proc(struct proc_param *param);

#endif
