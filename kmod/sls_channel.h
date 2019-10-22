#ifndef _SLS_CHANNEL_H_
#define _SLS_CHANNEL_H_

#include <sys/param.h>

#include <sys/uio.h>

#include <vm/vm_object.h>

/* 
 * A representation of an open channel into which we are going to dump the
 * checkpoint. Currently it can be either an open fd for a file, or an
 * open inode of the SLOS (also referred to as the OSD).
 */
struct sls_channel {
    int type;			/* The type of backend (SLS_FILE, SLS_OSD...) */
    union {
	struct file *fp;	/* The file pointer for SLS_FILE backends */
	struct slos_node *vp;	/* The SLOS vnode pointer for SLS_OSD backends */
    };

    uint64_t offset;		/* The offset into which we are writing. */
};

int sls_file_read(void* addr, size_t len, struct file *fp);
int sls_file_write(void* addr, size_t len, struct file *fp);

int sls_slos_read(void* addr, size_t len, uint64_t type, uint64_t *offset, struct slos_node *vp);
int sls_slos_write(void* addr, size_t len, uint64_t type, uint64_t *offset, struct slos_node *vp);

int sls_read(void *addr, size_t len, uint64_t rtype, struct sls_channel *chan);
int sls_write(void *addr, size_t len, uint64_t rtype, struct sls_channel *chan);

int slschan_init(struct sls_backend *sbak, struct sls_channel *chan);
void slschan_fini(struct sls_channel *chan);

int sls_dump(struct sls_process *slsp, struct sls_channel *chan);

#endif /* _SLS_CHANNEL_H_ */
