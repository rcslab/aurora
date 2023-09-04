#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/condvar.h>
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/libkern.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/pcpu.h>
#include <sys/rwlock.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/taskqueue.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>
#include <machine/vmparam.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>
#include <slos_inode.h>
#include <slos_io.h>
#include <slsfs.h>

#include "debug.h"
#include "slos_alloc.h"

/* We have only one SLOS currently. */
struct slos slos;

/* Block size for file-backed SLOSes. */
#define SLOS_FILEBLKSIZE (64 * 1024)

/* The SLOS can use as many physical buffers as it needs to. */
int slos_pbufcnt = -1;
uint64_t slos_io_initiated;
uint64_t slos_io_done;

static uma_zone_t slos_taskctx_zone;

MALLOC_DEFINE(M_SLOS_SB, "slos_superblock", "SLOS superblock");

struct slos_taskctx {
	struct task tk;
	struct vnode *vp;
	struct buf *bp;
};

int
slos_io_init(void)
{
	slos_taskctx_zone = uma_zcreate("SLOS io task contexts",
	    sizeof(struct slos_taskctx), NULL, NULL, NULL, NULL,
	    UMA_ALIGNOF(struct slos_taskctx), 0);
	if (slos_taskctx_zone == NULL)
		return (ENOMEM);

	return (0);
}

void
slos_io_uninit(void)
{
	uma_zdestroy(slos_taskctx_zone);
}

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

	/*
	 * Each superblock is 512 bytes long. Reading
	 * in using 4K sized blocks just brings in multiple
	 * blocks at a time. Overlapping reads do not compromise
	 * correctness.
	 */
	error = bread(
	    slos->slos_vp, index, DEV_BSIZE, curthread->td_proc->p_ucred, &bp);
	if (error != 0) {
		printf("bread failed with %d", error);
		return (error);
	}

	memcpy(sb, bp->b_data, sizeof(struct slos_sb));
	brelse(bp);

	/* Check whether the read superblock has sane values. */

	/* Check each superblock whether it is valid. */
	if (sb->sb_magic != SLOS_MAGIC) {
		return (EINVAL);
	}

	/* Sanity check the block size. */
	if (sb->sb_bsize != slos->slos_bsize)
		return (EINVAL);

	return (0);
}

/*
 * Read the superblock of the SLOS into the in-memory struct.
 * Device lock is held previous to call
 * */
int
slos_sbread(struct slos *slos)
{
	struct slos_sb *sb;
	struct stat st;
	int error;

	uint64_t largestepoch_i = 0;
	uint64_t largestepoch = 0;

	/*
	 * Do not allow regular file nodes to back the SLOS. If the user
	 * wants to they can use the file to back a ramdisk.
	 */
	if (slos->slos_vp->v_type == VREG)
		return (EINVAL);

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
	/* We assume that all superblocks have the same block size. */
	slos->slos_bsize = st.st_blksize;
	KASSERT(st.st_blksize != DEV_BSIZE, ("SLOS block size is one sector"));

	/* Find the largest epoch superblock in the NUMSBS array.
	 * This is starts at 0 offset of every device.
	 */
	for (int i = 0; i < NUMSBS; i++) {
		/* If we didn't find any, go check the next one. */
		error = slos_sbat(slos, i, sb);
		if (error != 0) {
			DEBUG1("ERROR: Superblock %d missing or corrupted", i);
			continue;
		}

		/* We found the last superblock. */
		if (sb->sb_epoch == EPOCH_INVAL && i != 0)
			break;

		DEBUG2("Superblock at %lu is epoch %d", i, sb->sb_epoch);
		if (sb->sb_epoch > largestepoch) {
			largestepoch = sb->sb_epoch;
			largestepoch_i = i;
		}
	}

	/* Last superblock we read was invalid, reread the last valid one. */
	error = slos_sbat(slos, largestepoch_i, sb);
	if (error != 0) {
		free(sb, M_SLOS_SB);
		return (error);
	}

	/* If we hit this case we didn't find a single valid block. */
	if (sb->sb_magic != SLOS_MAGIC) {
		printf("ERROR: Magic for SLOS is %lx, should be %lx\n",
		    sb->sb_magic, SLOS_MAGIC);
		free(sb, M_SLOS_SB);
		return (EINVAL);
	}

	DEBUG1("Largest superblock at %lu", largestepoch_i);
	DEBUG1("Checksum tree at %lu", sb->sb_cksumtree.offset);
	DEBUG1("Inodes File at %lu", sb->sb_root.offset);

	/* Make the superblock visible to the struct. */
	MPASS(sb->sb_index == largestepoch_i);
	return (0);
}

/*
 * Trim a new disk pointer that points to the physical blocks backing logical
 * block bln to start from the physical block pointing to newblkn instead.
 * Adjust its size accordingly.
 */
void
slos_ptr_trimstart(
    uint64_t newbln, uint64_t bln, size_t blksize, diskptr_t *ptr)
{
	int off;

	if (bln == newbln)
		return;

	off = newbln - bln;

	KASSERT(off > 0,
	    ("Physical extent %lu of size %lu cannot move to %lu", bln,
		ptr->size, newbln));
	KASSERT(off * blksize < ptr->size,
	    ("Cannot trim %lu bytes off a "
	     "%lu byte extent (blksize %lu)",
		off * blksize, ptr->size, blksize));
	KASSERT(ptr->size % blksize == 0,
	    ("Size of physical extent %lu bytes "
	     "not aligned to block size %lu",
		ptr->size, blksize));

	ptr->offset += off;
	ptr->size -= off * blksize;
}

/* Do the necessary btree manipulations before initiating an IO. */
static int __attribute__((noinline))
slos_io_setdaddr(struct slos_node *svp, size_t size, struct buf *bp)
{
	struct fbtree *tree = &svp->sn_tree;
	struct slos_diskptr ptr;
	size_t end;
	int error;

	BTREE_LOCK(tree, LK_EXCLUSIVE);
	VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);

	error = fbtree_rangeinsert(tree, bp->b_lblkno, size);
	if (error != 0)
		goto error;

	end = (bp->b_lblkno * IOSIZE(svp)) + size;
	if (svp->sn_ino.ino_size < end) {
		svp->sn_ino.ino_size = end;
		svp->sn_status |= SLOS_DIRTY;
	}

	error = slos_blkalloc(svp->sn_slos, size, &ptr);
	if (error != 0)
		goto error;

	KASSERT(ptr.size == size,
	    ("requested %lu bytes on disk, got %lu", ptr.size, size));

	/* No need to free the block if we fail, it'll get GCed. */
	error = fbtree_replace(&svp->sn_tree, &bp->b_lblkno, &ptr);
	if (error != 0)
		goto error;

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);

	/* Set the buffer to point to the physical block. */
	bp->b_blkno = ptr.offset;
	KASSERT(bp->b_resid <= ptr.size,
	    ("Doing %lu bytes of IO in a %lu segment", bp->b_resid, ptr.size));

	return (0);

error:
	bp->b_flags |= B_INVAL;
	bp->b_error = error;

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);
	return (error);
}

static int
slos_io_getdaddr(struct slos_node *svp, struct buf *bp)
{
	int error;
	struct slos_diskptr ptr;
	struct fnode_iter iter;
	struct fbtree *tree = &svp->sn_tree;
	daddr_t lblkno;

	atomic_add_64(&slos_io_initiated, 1);
	VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);
	BTREE_LOCK(&svp->sn_tree, LK_SHARED);

	/* Get the physical segment from where we will read. */
	error = fbtree_keymin_iter(&svp->sn_tree, &bp->b_lblkno, &iter);
	if (error != 0)
		goto error;

	/* Adjust the physical pointer to start where the buffer points. */
	lblkno = ITER_KEY_T(iter, uint64_t);
	KASSERT(!ITER_ISNULL(iter),
	    ("could not find logical offset %lu on disk", lblkno));
	ptr = ITER_VAL_T(iter, diskptr_t);
	slos_ptr_trimstart(bp->b_lblkno, lblkno, SLOS_BSIZE(slos), &ptr);

	KASSERT(bp->b_bcount <= ptr.size,
	    ("Reading %lu bytes from a physical "
	     "segment with %lu bytes",
		bp->b_bcount, ptr.size));

	bp->b_blkno = ptr.offset;

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);

	return (0);

error:
	bp->b_flags |= B_INVAL;
	bp->b_error = error;

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);
	return (error);
}

/*
 * Set up the physical on-disk address for the IO.
 */
static void __attribute__((noinline))
slos_io_physaddr(struct buf *bp, struct slos *slos)
{
	bp->b_flags &= ~B_INVAL;

	bp->b_rcred = crhold(curthread->td_ucred);
	bp->b_wcred = crhold(curthread->td_ucred);

	/* Scale the block number into device blocks. */
	bp->b_blkno = bp->b_blkno * (SLOS_BSIZE(*slos) / SLOS_DEVBSIZE(*slos));
	bp->b_iooffset = dbtob(bp->b_blkno);
}

/* Perform an IO without copying from the VM objects to the buffer. */
static void
slos_io(void *ctx, int __unused pending)
{
	struct slos_taskctx *task = (struct slos_taskctx *)ctx;
	struct vnode *vp = task->vp;
	struct slos_node *svp = SLSVP(task->vp);
	struct buf *bp = task->bp;
	size_t iosize = bp->b_resid;
	int iocmd = bp->b_iocmd;
	int error;

	KASSERT(iocmd == BIO_READ || iocmd == BIO_WRITE,
	    ("invalid IO type %d", iocmd));
	KASSERT(IOSIZE(svp) == PAGE_SIZE,
	    ("IOs are not page-sized (size %lu)", IOSIZE(svp)));
	KASSERT(SLOS_BSIZE(slos) >= SLOS_DEVBSIZE(slos),
	    ("FS bsize %lu > device bsize %lu", SLOS_BSIZE(slos),
		SLOS_DEVBSIZE(slos)));
	KASSERT((SLOS_BSIZE(slos) % SLOS_DEVBSIZE(slos)) == 0,
	    ("FS bsize %lu not multiple of device bsize %lu", SLOS_BSIZE(slos),
		SLOS_DEVBSIZE(slos)));
	KASSERT(bp->b_bcount > 0, ("IO of size 0"));

	BUF_ASSERT_LOCKED(bp);

	if (iocmd == BIO_WRITE) {
		/* Create the physical segment backing the write. */
		error = slos_io_setdaddr(svp, bp->b_resid, bp);
		if (error != 0) {
			printf("ERROR: IO failed with %d\n", error);
			goto out;
		}

		/* Update the size at the vnode and inode layers. */
		if (IDX_TO_OFF(bp->b_lblkno) + iosize > SLSINO(svp).ino_size) {
			SLSINO(svp).ino_size = IDX_TO_OFF(bp->b_lblkno) +
			    iosize;
			vnode_pager_setsize(vp, SLSINO(svp).ino_size);
		}
	} else if (iocmd == BIO_READ) {
		/* Retrieve the physical segment backing the read. */
		error = slos_io_getdaddr(svp, bp);
		if (error != 0) {
			printf("ERROR: IO failed with %d\n", error);
			goto out;
		}
	}

	slos_io_physaddr(bp, &slos);
	/*
	 * Test again, we have seen corruption in the past due to
	 * multiple entities using the buffer at the same time.
	 */
	KASSERT(bp->b_blkno <= slos.slos_sb->sb_size *
		    (SLOS_BSIZE(slos) / SLOS_DEVBSIZE(slos)),
	    ("buffer has invalid physical address %lx, maximum is %lx",
		bp->b_blkno,
		slos.slos_sb->sb_bsize *
		    (SLOS_BSIZE(slos) / SLOS_DEVBSIZE(slos))));

	g_vfs_strategy(&slos.slos_vp->v_bufobj, bp);

	/*
	 * Wait for the buffer to be done. Because the task calling
	 * slos_io() only exits after bwait() returns, waiting for the
	 * taskqueue to be drained is equivalent to waiting until all data has
	 * hit the disk.
	 */
	error = bufwait(bp);
	if (error != 0)
		DEBUG1("ERROR: bufwait returned %d", error);

	atomic_add_64(&slos_io_done, 1);
out:
	BUF_ASSERT_LOCKED(bp);
	relpbuf(bp, &slos_pbufcnt);

	vrele(vp);

	uma_zfree(slos_taskctx_zone, task);
}

static struct slos_taskctx *
slos_iotask_init(struct vnode *vp, struct buf *bp)
{
	struct slos_taskctx *ctx;

	ctx = uma_zalloc(slos_taskctx_zone, M_WAITOK | M_ZERO);
	*ctx = (struct slos_taskctx) {
		.vp = vp,
		.bp = bp,
	};

	return (ctx);
}

int
slos_iotask_create(struct vnode *vp, struct buf *bp, bool async)
{
	struct slos_taskctx *ctx;

	KASSERT(bp->b_resid > 0, ("IO of size 0"));
	ctx = slos_iotask_init(vp, bp);

	BUF_ASSERT_LOCKED(bp);

	/* The corresponding vrele() is in the task. */
	vref(vp);

	if (async) {
		TASK_INIT(&ctx->tk, 0, &slos_io, ctx);
		BUF_KERNPROC(bp);
		taskqueue_enqueue(slos.slos_tq, &ctx->tk);
	} else {
		slos_io(ctx, 0);
	}

	return (0);
}

boolean_t
slos_hasblock(struct vnode *vp, uint64_t lblkno_req, int *rbehind, int *rahead)
{
	struct fbtree *tree = &SLSVP(vp)->sn_tree;
	struct fnode_iter iter;
	struct slos_diskptr ptr;
	uint64_t lblkstart;
	uint64_t lblkno;
	boolean_t ret;
	int error;

	if (rbehind != NULL)
		*rbehind = 0;
	if (rahead != NULL)
		*rahead = 0;

	VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);
	BTREE_LOCK(tree, LK_SHARED);

	/* Get the physical segment from where we will read. */
	lblkstart = lblkno_req;
	error = fbtree_keymin_iter(tree, &lblkstart, &iter);
	if (error != 0) {
		printf("WARNING: Failed to look up swapped out page\n");
		ret = FALSE;
		goto out;
	}

	/* We do not have an infimum. */
	if (ITER_ISNULL(iter)) {
		ret = FALSE;
		goto out;
	}

	lblkno = ITER_KEY_T(iter, uint64_t);
	ptr = ITER_VAL_T(iter, diskptr_t);
	KASSERT(ptr.size > 0, ("found extent of size 0"));
	KASSERT(lblkno <= lblkstart,
	    ("keymin returned start %lu for lblkstart %lu", lblkno, lblkstart));

	/* Check if our infimum includes us. */
	if (lblkno + (ptr.size / PAGE_SIZE) <= lblkno_req) {
		ret = FALSE;
		goto out;
	}

	ret = TRUE;
	if (rbehind != NULL)
		*rbehind = imax((lblkno_req - lblkno), 0);
	if (rahead != NULL)
		*rahead = imax(
		    (lblkno + (ptr.size / PAGE_SIZE) - 1 - lblkno_req), 0);

out:
	BTREE_UNLOCK(tree, 0);
	VOP_UNLOCK(tree->bt_backend, 0);

	return (ret);
}
