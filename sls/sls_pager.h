#ifndef _SLS_SWAPPER_H_
#define _SLS_SWAPPER_H_

void slsvm_pager_register(void);
struct buf *sls_swap_getreadbuf(
    vm_object_t obj, vm_pindex_t pindex, size_t npages);
void sls_pager_register(void);
struct buf *sls_pager_readbuf(
    vm_object_t obj, vm_pindex_t pindex, size_t npages, bool *retry);
struct buf *sls_pager_writebuf(
    vm_object_t obj, vm_pindex_t pindex, size_t targetsize, bool *retry);

void sls_pager_unregister(void);
void sls_pager_swapoff(void);

int sls_pager_obj_init(vm_object_t obj);

#define b_aurobj b_fsprivate2

#endif /* _SLS_SWAPPER_H_ */
