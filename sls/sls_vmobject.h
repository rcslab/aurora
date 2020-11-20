#ifndef _SLS_VMOBJECT_H_
#define _SLS_VMOBJECT_H_

#include <sys/types.h>

#include <sys/sbuf.h>

#include "sls_internal.h"
#include "sls_data.h"
#include "sls_kv.h"
#include "sls_load.h"
#include "sls_table.h"

int slsckpt_vmobject(vm_object_t obj, struct slsckpt_data *sckpt_data);
int slsckpt_vmobject_shm(vm_object_t *objp, struct slsckpt_data *sckpt_data);
int slsrest_vmobject(struct slsvmobject *slsvmobject, struct slsrest_data *restdata);

#endif /* _SLS_VMOBJECT_H_ */
