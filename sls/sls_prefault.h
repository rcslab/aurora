#ifndef _SLS_PREFAULT_H_
#define _SLS_PREFAULT_H_

#include <sys/param.h>
#include <sys/bitstring.h>

#include <vm/vm.h>
#include <vm/vm_object.h>

struct sls_prefault {
	uint64_t pre_slsid;
	size_t pre_size;   /* Size of the bitmap in _bits_. */
	bitstr_t *pre_map; /* The bitmap itself. */
};

int slspre_create(uint64_t slsid, size_t size, struct sls_prefault **slsprep);
void slspre_destroy(struct sls_prefault *slsprep);

int slspre_vector_populated(uint64_t objid, vm_object_t obj);
int slspre_vector_empty(
    uint64_t objid, size_t size, struct sls_prefault **slsprep);

void slspre_mark(uint64_t prefaultid, vm_pindex_t start, vm_pindex_t stop);

int slspre_export(void);
int slspre_import(void);

int slspre_vnode(struct vnode *vp, struct sls_attr attr);
#endif /* _SLS_PREFAULT_H_ */
