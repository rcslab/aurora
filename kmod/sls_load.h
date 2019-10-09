#ifndef _SLS_RESTORE_H_
#define _SLS_RESTORE_H_

#include "slskv.h"
#include "sls_process.h"

int sls_load_thread(struct thread_info *thread_info, char **bufp, size_t *bufsizep);
int sls_load_proc(struct proc_info *proc_info, char **bufp, size_t *bufsizep);
int sls_load_file(struct file_info *file, void **data, char **bufp, size_t *bufsizep);
int sls_load_filedesc(struct filedesc_info *filedesc, char **bufp, size_t *bufsizep);
int sls_load_vmobject(struct vm_object_info *obj, char **bufp, size_t *bufsizep);
int sls_load_vmentry(struct vm_map_entry_info *entry, char **bufp, size_t *bufsizep);
int sls_load_memory(struct memckpt_info *memory, char **bufp, size_t *bufsizep);
int sls_load_path(struct sbuf **sbp, char **bufp, size_t *bufsizep); 

#endif /* _SLS_RESTORE_H_ */
