#ifndef _SLS_VMOBJECT_H_
#define _SLS_VMOBJECT_H_

#include <sys/types.h>
#include <sys/sbuf.h>

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_kv.h"
#include "sls_load.h"
#include "sls_table.h"

int slsvmobj_checkpoint(vm_object_t obj, struct slsckpt_data *sckpt_data);
int slsvmobj_checkpoint_shm(vm_object_t *objp, struct slsckpt_data *sckpt_data);
int slsvmobj_restore_all(
    struct slskv_table *rectable, struct slsrest_data *restdata);

#endif /* _SLS_VMOBJECT_H_ */
