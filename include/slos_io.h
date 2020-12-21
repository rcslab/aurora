#ifndef _SLOS_IO_H_
#define _SLOS_IO_H_

#include <sys/file.h>

#include <slos.h>

#ifndef _KERNEL
#error "No serviceable parts inside"
#endif /* _KERNEL */

struct buf;

/* XXX We won't need this here when we use direct buffer IOs from the SLS.  */
void slos_uioinit(struct uio *auio, uint64_t off, enum uio_rw rwflag, 
	struct iovec *aiovs, size_t iovcnt);

int slos_sbread(struct slos *slos);
int slos_sbat(struct slos *slos, int index, struct slos_sb *sb);

/* Direct SLOS IO. */
int slos_iotask_create(struct vnode *vp, struct buf *bp, bool async);
boolean_t slos_hasblock(struct vnode *vp, uint64_t lblkno_req, int *rbehind, int *rahead);

int slos_io_init(void);
void slos_io_uninit(void);

extern int slos_pbufcnt;

#endif /* _SLOS_IO_H_ */
