#ifndef _SLS_H__
#define _SLS_H__

#include "sls_ioctl.h"

int sls_dump(struct dump_param *param);
int sls_restore(struct restore_param *param);

#endif
