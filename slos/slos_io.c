#include <sys/param.h>
#include <sys/lock.h>

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
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/rwlock.h>
#include <sys/syscallsubr.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/vnode.h>
#include <sys/uio.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <machine/atomic.h>
#include <machine/vmparam.h>

#include <slos_inode.h>
#include <slos_io.h>
#include <slsfs.h>

#include "slos_alloc.h"

#include "debug.h"

/* We have only one SLOS currently. */
struct slos slos;

/* Block size for file-backed SLOSes. */
#define SLOS_FILEBLKSIZE (64 * 1024)

/* The SLOS can use as many physical buffers as it needs to. */
static int slos_pbufcnt = -1;

static MALLOC_DEFINE(M_SLOS_IO, "slos_io", "SLOS IO context");
MALLOC_DEFINE(M_SLOS_SB, "slos_superblock", "SLOS superblock");

struct slos_taskctx {
	struct task tk;
	struct slos_node *svp;
	vm_object_t obj;
	slos_callback cb;
	vm_page_t startm;
	int iotype;
	uint64_t size;
};

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
		free(sb, M_SLOS_SB);
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
			free(sb, M_SLOS_SB);

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
			free(sb, M_SLOS_SB);
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
		free(sb, M_SLOS_SB);
		return (error);
	}

	if (sb->sb_magic != SLOS_MAGIC) {
		printf("ERROR: Magic for SLOS is %lx, should be %llx",
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
slos_ptr_trimstart(uint64_t newbln, uint64_t bln, size_t blksize, diskptr_t *ptr) 
{
	int off;

	if (bln == newbln)
		return;

	off = newbln - bln;

	KASSERT(off > 0, ("Physical extent %lu of size %lu cannot move to %lu",
	    bln, ptr->size, newbln));
	KASSERT(off * blksize < ptr->size, ("Cannot trim %lu bytes off a "
	    "%lu byte extent (blksize %lu)", off * blksize, ptr->size, blksize));
	KASSERT(ptr->size % blksize == 0, ("Size of physical extent %lu bytes "
	    "not aligned to block size %lu", ptr->size, blksize));

	ptr->offset += off;
	ptr->size -= off * blksize;
}

/*
 * Attach the pages to be read in or out into the buffer.
 */
static void
slos_io_pgattach(vm_object_t obj, vm_page_t startm, int targetsize, struct buf *bp)
{
	vm_page_t m;
	size_t size = 0;
	int npages = 0;

	VM_OBJECT_RLOCK(obj);
	m = startm;
	TAILQ_FOREACH_FROM(m, &obj->memq, listq) {
		KASSERT(npages < btoc(MAXPHYS), ("overran b_pages[] array"));
		KASSERT(obj->ref_count >= obj->shadow_count + 1,
		    ("object has %d references and %d shadows",
		    obj->ref_count, obj->shadow_count));
		KASSERT(m->object == obj, ("page %p in object %p "
		    "associated with object %p",
		    m, obj, m->object));
		KASSERT(startm->pindex + npages == m->pindex,
		    ("pages in the buffer are not consecutive "
		    "(pindex should be %ld, is %ld)",
		    startm->pindex + npages, m->pindex));

		bp->b_pages[npages++] = m;
		KASSERT(pagesizes[m->psind] <= PAGE_SIZE,
		    ("dumping page %p with size %ld", m, pagesizes[m->psind]));
		size += pagesizes[m->psind];
		/*  We are done grabbing pages. */
		if (size == targetsize)
			break;
	}
	VM_OBJECT_RUNLOCK(obj);
	KASSERT((targetsize / PAGE_SIZE) < btoc(MAXPHYS), ("IO too large"));

	/* Update the buffer to point to the pages. */
	bp->b_npages = npages;
	KASSERT(bp->b_npages > 0, ("no pages in the IO"));

	/*
	 * XXX Check if doing runningbufspace accounting causes performance
	 * problems. If so, increase it.
	 */
	bp->b_bcount = bp->b_bufsize = bp->b_runningbufspace = targetsize;
	bp->b_resid = targetsize;
	atomic_add_long(&runningbufspace, bp->b_runningbufspace);
}

/* Do the necessary btree manipulations before initiating an IO. */
static int
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

	KASSERT(ptr.size == size, ("requested %lu bytes on disk, got %lu", 
	    ptr.size, size));

	/* No need to free the block if we fail, it'll get GCed. */
	error = fbtree_replace(&svp->sn_tree, &bp->b_lblkno, &ptr);
	if (error != 0)
		goto error;

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);

	/* Set the buffer to point to the physical block. */
	bp->b_blkno = ptr.offset;
	KASSERT(bp->b_resid == ptr.size, ("Doing %lu bytes of IO in a %lu segment",
	    bp->b_resid, ptr.size));

	return (0);

error:
	bp->b_flags |= B_INVAL;
	bp->b_error = error;

	VOP_UNLOCK(tree->bt_backend, 0);
	BTREE_UNLOCK(tree, 0);
	return (error);

}

static int
slos_io_getdaddr(struct slos_node *svp, size_t size, struct buf *bp)
{
	struct slos_diskptr ptr;
	struct fnode_iter iter;
	struct fbtree *tree = &svp->sn_tree;
	daddr_t lblkno = bp->b_lblkno;
	int error;

	VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);
	BTREE_LOCK(&svp->sn_tree, LK_SHARED);

	/* Get the physical segment from where we will read. */
	error = fbtree_keymin_iter(&svp->sn_tree, &bp->b_lblkno, &iter);
	if (error != 0)
		goto error;

	/* Adjust the physical pointer to start where the buffer points. */
	lblkno = ITER_KEY_T(iter, uint64_t);
	KASSERT(!ITER_ISNULL(iter), ("could not find logical offset %lu on disk", lblkno));
	ptr = ITER_VAL_T(iter, diskptr_t);
	slos_ptr_trimstart(bp->b_lblkno, lblkno, SLOS_BSIZE(slos), &ptr);

	KASSERT(bp->b_bcount <= ptr.size, ("Reading %lu bytes from a physical "
	    "segment with %lu bytes", bp->b_bcount, ptr.size));

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

/* Perform an IO without copying from the VM objects to the buffer. */
static void
slos_io(void *ctx, int pending)
{
	struct buf *bp;
	struct slos_taskctx *task = (struct slos_taskctx *)ctx;
	struct slos_node *svp = task->svp;
	int iotype = task->iotype;
	vm_object_t obj = task->obj;
	vm_page_t startm = task->startm;
	daddr_t lblkno;
	int error, i;

	KASSERT(iotype == BIO_READ || iotype == BIO_WRITE, ("invalid IO type %d", iotype));
	ASSERT_VOP_UNLOCKED(vp, ("vnode %p is locked", vp));

	if (task->tk.ta_func == NULL)
		DEBUG1("Warning: %s called synchronously", __func__);
	KASSERT(IOSIZE(svp) == PAGE_SIZE,  ("IOs are not page-sized (size %lu)", IOSIZE(svp)));


	/* The FS and device block sizes are needed below. */
	KASSERT(SLOS_BSIZE(slos) >= SLOS_DEVBSIZE(slos), ("FS bsize %lu > device bsize %lu",
	    SLOS_BSIZE(slos), SLOS_DEVBSIZE(slos)));
	KASSERT((SLOS_BSIZE(slos) % SLOS_DEVBSIZE(slos)) == 0,
	    ("FS bsize %lu not multiple of device bsize %lu",
	    SLOS_BSIZE(slos), SLOS_DEVBSIZE(slos)));

	/* If the IO would be empty, this function is a no-op.*/
	if (task->size == 0) {
		vm_object_deallocate(obj);
		free(task, M_SLOS_IO);
		return;
	}

	/* Set the logical block address of the buffer. */
	lblkno = startm->pindex + SLOS_OBJOFF;
	bp = getpbuf(&slos_pbufcnt);
	bp->b_lblkno = lblkno;

	DEBUG4("%s:%d: iotype %d for bp %p", __FILE__, __LINE__, iotype, bp);

	/* Attach the pages to the buffer, set the size accordingly. */
	slos_io_pgattach(obj, startm, task->size, bp);

	if (iotype == BIO_WRITE) {
		/* Create the physical segment backing the write. */
		error = slos_io_setdaddr(svp, task->size, bp);
		if (error != 0) {
			printf("ERROR: IO failed with %d\n", error);
			goto out;
		}
	} else if (iotype == BIO_READ) {
		/* Retrieve the physical segment backing the read. */
		error = slos_io_getdaddr(svp, task->size, bp);
		if (error != 0) {
			printf("ERROR: IO failed with %d\n", error);
			goto out;
		}
	} else {
		panic("Invalid iotype %d", iotype);
	}

	/* We only map the buffer if we need to read. */
	DEBUG4("%s:%d: buf %p has %d pages", __FILE__, __LINE__, bp, bp->b_npages);

	bp->b_iocmd = iotype;
	bp->b_iodone = bdone;

	bp->b_flags &= ~B_INVAL;

	bp->b_rcred = crhold(curthread->td_ucred);
	bp->b_wcred = crhold(curthread->td_ucred);

	/*
	 * XXX Do we need to lock the device vnode for the write? That doesn't
	 * seem sensible.
	 */

	/* Scale the block number into device blocks. */
	bp->b_blkno = bp->b_blkno * (SLOS_BSIZE(slos) / SLOS_DEVBSIZE(slos));
	bp->b_iooffset = dbtob(bp->b_blkno);

	bp->b_data = unmapped_buf;
	bp->b_offset = 0;

	g_vfs_strategy(&slos.slos_vp->v_bufobj, bp);

	/*
	 * Wait for the buffer to be done. Because the task calling
	 * slos_io() only exits after bwait() returns, waiting for the
	 * taskqueue to be drained is equivalent to waiting until all data has
	 * hit the disk.
	 */
	bwait(bp, PRIBIO, "slsrw");
	KASSERT(bp->b_resid == 0, ("Still have %lu bytes of IO left", bp->b_resid));

	/* Scale the block number back to filesystem blocks.*/
	bp->b_blkno = bp->b_blkno / (SLOS_BSIZE(slos) / SLOS_DEVBSIZE(slos));

out:
	/*
	 * Make sure the buffer is still in a sane state after the IO has
	 * completed. Remove all pages from the buffer.
	 */
	KASSERT(bp->b_npages != 0, ("buffer has no pages"));
	for (i = 0; i < bp->b_npages; i++) {
		KASSERT(bp->b_pages[i]->object != 0, ("page without an associated object found"));
		KASSERT(bp->b_pages[i]->object == obj, ("page %p in object %p "
		    "associated with object %p",
		    bp->b_pages[i], obj, bp->b_pages[i]->object));
		bp->b_pages[i] = bogus_page;
	}
	bp->b_npages = 0;
	bp->b_bcount = bp->b_bufsize = 0;

	/*
	 * Free the reference to the object taken when creating the task. VM 
	 * object deallocation may sleep, so we cannot do it from the buffer's 
	 * callback routine.
	 */
	vm_object_deallocate(obj);
	relpbuf(bp, &slos_pbufcnt);
	free(task, M_SLOS_IO);
}

static struct slos_taskctx *
slos_iotask(struct slos_node *svp,  vm_object_t obj, vm_page_t m, size_t len, int iotype, slos_callback cb)
{
	struct slos_taskctx *ctx;

	ctx = malloc(sizeof(*ctx), M_SLOS_IO, M_WAITOK | M_ZERO);
	*ctx = (struct slos_taskctx) {
		.svp = svp,
		.obj = obj,
		.startm = m,
		.size = len,
		.iotype = iotype,
		.cb = cb,
	};

	return (ctx);
}

int
slos_iotask_create(struct slos_node *svp, vm_object_t obj, vm_page_t m,
    size_t len, int iotype, slos_callback cb, bool async)
{
	struct slos_taskctx *ctx;

	/* Ignore IOs of size 0. */
	if (len == 0)
		return (0);

	/*
	 * Get a reference to the object, freed when the buffer is done. We have
	 * to do this here, if we do it in the task the page might be dead by
	 * then.
	 */
	vm_object_reference(obj);
	ctx = slos_iotask(svp, obj, m, len, iotype, cb);

	if (async) {
		TASK_INIT(&ctx->tk, 0, &slos_io, ctx);
		taskqueue_enqueue(slos.slos_tq, &ctx->tk);
	} else {
		slos_io(ctx, 0);
	}

	return (0);
}
