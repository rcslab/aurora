#ifndef _SLS_OBJTABLE_H_
#define _SLS_OBJTABLE_H_

#include <vm/vm_object.h>

#define HASH_MAX (4 * 1024)

struct vmobj_pair {
    vm_object_t original;
    vm_object_t restored; 
    LIST_ENTRY(vmobj_pair) next;
};

LIST_HEAD(vmobj_pairs, vmobj_pair);

struct sls_objtable {
    struct vmobj_pairs *objects;
    u_long hashmask;
};

int sls_objtable_init(struct sls_objtable *objtable);
void sls_objtable_fini(struct sls_objtable *objtable);
vm_object_t sls_objtable_find(vm_object_t obj, struct sls_objtable *objtable);
void sls_objtable_add(vm_object_t original, vm_object_t restored, struct sls_objtable *objtable);

#endif /* _SLS_OBJTABLE_H_ */
