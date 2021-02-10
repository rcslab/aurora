#ifndef _SLS_LOAD_H_
#define _SLS_LOAD_H_

#include <sls_data.h>

#include "sls_kv.h"

/*
 * XXX These calls can be refactored as preprocessor wrappers around a simple
 * function that takes the size and the magic number to check against (we can
 * assume that it is always the first field of the struct that is the magic
 * number, so we can typecast to a uint64_t).
 */
int slsload_thread(struct slsthread *slsthread, char **bufp, size_t *bufsizep);
int slsload_proc(struct slsproc *slsproc, char **bufp, size_t *bufsizep);
int slsload_vmobject(struct slsvmobject *obj, char **bufp, size_t *bufsizep);
int slsload_vmentry(struct slsvmentry *entry, char **bufp, size_t *bufsizep);
int slsload_vnode(struct slsvnode *slsvnode, char **bufp, size_t *bufsizep);
int slsload_sysvshm(struct slssysvshm *shm, char **bufp, size_t *bufsizep);

int slsload_file(
    struct slsfile *file, void **data, char **bufp, size_t *bufsizep);
int slsload_filedesc(struct slsfiledesc *filedesc, char **bufp,
    size_t *bufsizep, struct slskv_table **fdtable);
int slsload_vmspace(struct slsvmspace *vm, struct shmmap_state **shmstate,
    char **bufp, size_t *bufsizep);
int slsload_sockbuf(
    struct mbuf **mp, uint64_t *sbid, char **bufp, size_t *bufsizep);

#endif /* _SLS_LOAD_H_ */
