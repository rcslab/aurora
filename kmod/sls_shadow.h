#ifndef _SLS_SHADOW_H_
#define _SLS_SHADOW_H_

#include <sys/proc.h>
#include <vm/vm_object.h>

vm_object_t *sls_shadow(struct proc *p, size_t *numobjects);
void sls_compact(vm_object_t *objects, size_t numobjects);

#endif /* _SLS_SHADOW_H_ */
