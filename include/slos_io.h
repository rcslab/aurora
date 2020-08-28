#ifndef _SLOS_IO_H_
#define _SLOS_IO_H_

#include <sys/file.h>

#include <slos.h>

#ifdef _KERNEL
void slos_uioinit(struct uio *auio, uint64_t off, enum uio_rw rwflag, 
	struct iovec *aiovs, size_t iovcnt);

int slos_sbread(struct slos *slos);

int slos_sbat(struct slos *slos, int index, struct slos_sb *sb);

int slos_readblk(struct slos *slos, uint64_t blkno, void *buf);
int slos_writeblk(struct slos *slos, uint64_t blkno, void *buf);
#endif

#endif /* _SLOS_IO_H_ */
