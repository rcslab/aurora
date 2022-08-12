#ifndef _SLSVM_H_
#define _SLSVM_H_

#include <sys/types.h>
#include <sys/conf.h>
#include <sys/pcpu.h>
#include <sys/proc.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include "sls_internal.h"
#include "sls_kv.h"

#define OBJT_ISANONYMOUS(obj) \
	((obj != NULL) &&     \
	    ((obj->type == OBJT_DEFAULT) || (obj->type == OBJT_SWAP)))

int slsvm_entry_shadow(struct proc *p, struct slskv_table *table,
    vm_map_entry_t entry, bool is_fullckpt);
void slsvm_objtable_collapsenew(
    struct slskv_table *objtable, struct slskv_table *newtable);
void slsvm_objtable_collapse(
    struct slskv_table *objtable, struct slskv_table *newtable);
int slsvm_procset_shadow(
    slsset *procset, struct slsckpt_data *sckpt, bool is_fullckpt);
void slsvm_forceshadow(
    vm_object_t shadow, vm_object_t source, vm_ooffset_t offset);

void slsvm_object_reftransfer(vm_object_t src, vm_object_t dst);
int slsvm_object_shadow(struct slskv_table *objtable, vm_object_t *objp);
void slsvm_object_copy(
    struct proc *p, struct vm_map_entry *entry, vm_object_t obj);
void slsvm_object_precopy(vm_object_t object, vm_object_t parent);

void slsvm_print_chain(vm_object_t shadow);
void slsvm_print_crc32_vmspace(struct vmspace *vm);
void slsvm_print_crc32_object(vm_object_t obj);

void slsvm_print_vmobject(struct vm_object *obj);
void slsvm_print_vmspace(struct vmspace *space);
void slsvm_print_chain(vm_object_t shadow);
void slsvm_object_scan(void);

void slsvm_pages_dump(struct vmspace *vm, struct slskv_table *table);
void slsvm_pages_check(struct vmspace *vm, struct slskv_table *table);

#endif /* _SLSVM_H_ */
