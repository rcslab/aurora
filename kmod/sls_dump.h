#ifndef _SLS_DUMP_H_
#define _SLS_DUMP_H_

#include <sys/mman.h>

#include <sys/file.h>

#include <vm/vm_map.h>

#include "sls_process.h"

/* 
 * A representation of an open channel into which we are going to dump the
 * checkpoint. Currently it can be either an open fd for a file, or an
 * open inode of the SLOS (also referred to as the OSD).
 */
struct sls_channel {
    int type;			/* The type of backend (SLS_FILE, SLS_OSD...) */
    union {
	struct file *fp;	/* The file pointer for SLS_FILE backends */
	struct slos_vnode *vp;	/* The SLOS vnode pointer for SLS_OSD backends */
    };

    uint64_t offset;		/* The offset into which we are writing. */
};

int sls_dump(struct sls_process *slsp, struct sls_channel *chan);

#endif /* _SLS_DUMP_H_ */
