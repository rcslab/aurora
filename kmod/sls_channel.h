#ifndef _SLS_FILE_H_
#define _SLS_FILE_H_

#include <sys/param.h>

#include <sys/file.h>
#include <sys/uio.h>

#include <vm/vm_object.h>

#include "sls_mem.h"

int sls_file_read(void* addr, size_t len, struct file *fp);
int sls_file_write(void* addr, size_t len, struct file *fp);

int sls_slos_read(void* addr, size_t len, uint64_t type, uint64_t offset, struct slos_vnode *vp);
int sls_slos_write(void* addr, size_t len, uint64_t type, uint64_t offset, struct slos_vnode *vp);

int sls_write(void *addr, size_t len, struct sls_channel *chan);

int slschan_init(struct sls_backend *sbak, struct sls_channel *chan);
void slschan_fini(struct sls_channel *chan);

#endif /* _SLS_FILE_H */
