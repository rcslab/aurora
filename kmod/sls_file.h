#ifndef _SLSFILE_H_
#define _SLSFILE_H_

#include <sys/param.h>

#include <sys/file.h>
#include <vm/vm_object.h>

#include "sls_mem.h"

int file_read(void* addr, size_t len, int fd);
int file_write(void* addr, size_t len, int fd);
int file_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
		    size_t numentries, int fd, int mode);

#endif /* _SLSFILE_H */
