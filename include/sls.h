#ifndef _SLS_H__
#define _SLS_H__

#include "sls_ioctl.h"

int sls_snap(struct snap_param *param);
int sls_op(struct op_param *param);

#endif
