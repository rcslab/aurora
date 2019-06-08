#ifndef _SLS_DUMP_H_
#define _SLS_DUMP_H_

#include <sys/mman.h>

#include <vm/vm_map.h>

#include "sls_process.h"

struct sls_store_tgt {
    int type;
    union {
	int fd;
	struct sls_snapshot *slss;
    };
};

int sls_dump(struct sls_process *slsp, int mode, int fd);

#endif /* _SLS_DUMP_H_ */
