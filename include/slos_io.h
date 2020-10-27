#ifndef _SLOS_IO_H_
#define _SLOS_IO_H_

#include <sys/file.h>

#include <slos.h>

#ifndef _KERNEL
#error "No serviceable parts inside"
#endif /* _KERNEL */

/* XXX We won't need this here when we use direct buffer IOs from the SLS.  */
void slos_uioinit(struct uio *auio, uint64_t off, enum uio_rw rwflag, 
	struct iovec *aiovs, size_t iovcnt);

int slos_sbread(struct slos *slos);
int slos_sbat(struct slos *slos, int index, struct slos_sb *sb);

/* Direct SLOS IO. */
typedef void (*slos_callback)(void *context);
int slos_iotask_create(struct slos_node *svp, vm_object_t obj, vm_page_t m, 
	size_t len, int iotype, slos_callback cb, bool async);

#endif /* _SLOS_IO_H_ */
