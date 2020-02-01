#ifndef _SLS_SYSV_H_
#define _SLS_SYSV_H_

#include <sls_data.h>

#include "sls_kv.h"
#include "sls_internal.h"

int slsckpt_sysvshm(struct slsckpt_data *sckpt_data, struct slskv_table *objtable);
int slsrest_sysvshm(struct slssysvshm *slssysvshm, struct slskv_table *objtable);

#endif /* _SLS_SYSV_H_ */
