#ifndef _SLS_SYSV_H_
#define _SLS_SYSV_H_

#include <sls_data.h>

#include "sls_internal.h"
#include "sls_kv.h"

int slsckpt_sysvshm(struct slsckpt_data *sckpt);
int slsrest_sysvshm(struct slssysvshm *info, struct slskv_table *objtable);

#endif /* _SLS_SYSV_H_ */
