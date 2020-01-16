#ifndef _SLS_LOAD_H_
#define _SLS_LOAD_H_

#include <sls_data.h>

#include "imported_sls.h"
#include "sls_kv.h"

int slsload_thread(struct slsthread *thread_info, char **bufp, size_t *bufsizep);
int slsload_proc(struct slsproc *proc_info, struct sbuf **namep, char **bufp, size_t *bufsizep);
int slsload_file(struct slsfile *file, void **data, char **bufp, size_t *bufsizep);
int slsload_filedesc(struct slsfiledesc *filedesc, char **bufp, size_t *bufsizep, struct slskv_table **fdtable);
int slsload_vmobject(struct slsvmobject *obj, char **bufp, size_t *bufsizep);
int slsload_vmentry(struct slsvmentry *entry, char **bufp, size_t *bufsizep);
int slsload_vmspace(struct slsvmspace *vm, struct shmmap_state **shmstate, char **bufp, size_t *bufsizep);
int slsload_path(struct sbuf **sbp, char **bufp, size_t *bufsizep); 
int slsload_sysvshm(struct slssysvshm *shm, char **bufp, size_t *bufsizep);

#endif /* _SLS_LOAD_H_ */
