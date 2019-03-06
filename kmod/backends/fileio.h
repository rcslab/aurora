#ifndef _FILEIO_H_
#define _FILEIO_H_

#include <sys/types.h>

#include "../memckpt.h"
#include "desc.h"

int fd_read(void* addr, size_t len, struct sls_desc desc);
int fd_write(void* addr, size_t len, struct sls_desc desc);
void fd_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
		    size_t numentries, struct sls_desc desc);
void backends_init(void);
void backends_fini(void);

#endif
