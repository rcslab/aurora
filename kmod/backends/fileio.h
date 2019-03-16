#ifndef _FILEIO_H_
#define _FILEIO_H_

#include <sys/types.h>

#include "../memckpt.h"
#include "desc.h"

int fd_read(void* addr, size_t len, struct sls_desc *desc);
int fd_write(void* addr, size_t len, struct sls_desc *desc);
void fd_dump(struct vm_map_entry_info *entries, vm_object_t *objects, 
		    size_t numentries, struct sls_desc *desc, int mode);

void backends_init(void);
void backends_fini(void);

#define SLS_LOG_ENTRIES 10000
extern long sls_log[9][SLS_LOG_ENTRIES];
extern int sls_log_counter;

inline long 
tonano(struct timespec tv)
{
    const long billion = 1000L * 1000 * 1000;
    return billion * tv.tv_sec + tv.tv_nsec;
}

#endif
