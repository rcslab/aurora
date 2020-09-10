#include <sys/param.h>

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kthread.h>
#include <sys/libkern.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/syscallsubr.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/uio.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>
#include <machine/vmparam.h>

#include <slos_io.h>
#include <slsfs.h>

#include "slosmm.h"
#include "debug.h"


/* 
 * Initialize a UIO for operation rwflag at offset off,
 * and asssign an IO vector to the given UIO. 
 */
void
slos_uioinit(struct uio *auio, uint64_t off, enum uio_rw rwflag,
    struct iovec *aiov, size_t iovcnt)
{
	size_t len;
	int i;

	bzero(auio, sizeof(*auio));

	auio->uio_iov = NULL;
	auio->uio_offset = off;
	auio->uio_segflg = UIO_SYSSPACE;
	auio->uio_rw = rwflag;
	auio->uio_iovcnt = 0;
	auio->uio_resid = 0;
	auio->uio_td = curthread;

	for (len = 0, i = 0; i < iovcnt; i++)
		len += aiov[i].iov_len;

	auio->uio_iov = aiov;
	auio->uio_iovcnt = iovcnt;
	auio->uio_resid = len;
}

int
slos_sbat(struct slos *slos, int index, struct slos_sb *sb)
{
	struct buf *bp;
	int error;
	error = bread(slos->slos_vp, index, slos->slos_sb->sb_bsize, curthread->td_proc->p_ucred, &bp);
	if (error != 0) {
		free(sb, M_SLOS);
		printf("bread failed with %d", error);

		return error;
	}
	memcpy(sb, bp->b_data, sizeof(struct slos_sb));
	brelse(bp);

	return (0);
}
/* 
 * Read the superblock of the SLOS into the in-memory struct.  
 * Device lock is held previous to call
 * */
int
slos_sbread(struct slos * slos)
{
	struct slos_sb *sb;
	struct stat st;
	struct iovec aiov;
	struct uio auio;
	int error;
	
	uint64_t largestepoch_i = 0;
	uint64_t largestepoch = 0;

	/* If we're backed by a file, just call VOP_READ. */
	if (slos->slos_vp->v_type == VREG) {
		sb = malloc(SLOS_FILEBLKSIZE, M_SLOS_SB, M_WAITOK | M_ZERO);

		/* Read the first SLOS_FILEBLKSIZE bytes. */
		aiov.iov_base = sb;
		aiov.iov_len = SLOS_FILEBLKSIZE;
		slos_uioinit(&auio, 0,UIO_READ, &aiov, 1);

		/* Issue the read. */
		error = VOP_READ(slos->slos_vp, &auio, 0, curthread->td_ucred);
		if (error != 0) 
			free(sb, M_SLOS);

		/* Make the superblock visible. */
		slos->slos_sb = sb;

		return error;
	}

	/* 
	 * Our read and write routines depend on our superblock
	 * for information like the block size, so we can't use them.
	 * We instead do a stat call on the vnode to get it directly.
	 */
	error = vn_stat(slos->slos_vp, &st, NULL, NULL, curthread);
	if (error != 0) {
		printf("vn_stat failed with %d", error);
		return error;
	}

	sb = malloc(st.st_blksize, M_SLOS_SB, M_WAITOK | M_ZERO);
	slos->slos_sb = sb;
	sb->sb_bsize = st.st_blksize;

	/* Find the largest epoch superblock in the NUMSBS array.
	 * This is starts at 0  offset of every device
	 */
	for (int i = 0; i < NUMSBS; i++) {
		error = slos_sbat(slos, i, sb);
		if (error != 0) {
			free(sb, M_SLOS);
			return (error);
		}
		if (sb->sb_epoch == EPOCH_INVAL && i != 0) {
			break;
		}

		DEBUG2("Superblock at %lu is epoch %d", i, sb->sb_epoch);
		if (sb->sb_epoch > largestepoch) {
			largestepoch = sb->sb_epoch;
			largestepoch_i = i;
		}
	}

	error = slos_sbat(slos, largestepoch_i, sb);
	if (error != 0) {
		free(sb, M_SLOS);
		return (error);
	}

	if (sb->sb_magic != SLOS_MAGIC) {
		printf("ERROR: Magic for SLOS is %lx, should be %llx",
		    sb->sb_magic, SLOS_MAGIC);
		free(sb, M_SLOS);
		return (EINVAL);
	} 

	DEBUG1("Largest superblock at %lu", largestepoch_i);
	DEBUG1("Checksum tree at %lu", sb->sb_cksumtree.offset);
	DEBUG1("Inodes File at %lu", sb->sb_root.offset);

	/* Make the superblock visible to the struct. */
	MPASS(sb->sb_index == largestepoch_i);
	return (0);
}
