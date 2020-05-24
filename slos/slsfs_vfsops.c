#include <sys/param.h>
#include <machine/param.h>

#include <sys/systm.h>
#include <sys/types.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/unistd.h>
#include <sys/extattr.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/lockf.h>
#include <sys/module.h>
#include <sys/namei.h>
#include <sys/uio.h>
#include <sys/bio.h>
#include <sys/priv.h>
#include <sys/conf.h>
#include <sys/mutex.h>
#include <sys/rwlock.h>
#include <sys/kthread.h>
#include <sys/stat.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/taskqueue.h>
#include <sys/pctrie.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_page.h>

#include <slos.h>
#include <slos_record.h>
#include <slos_inode.h>
#include <slos_btree.h>
#include <slos_io.h>
#include <slosmm.h>
#include <btree.h>

#include "slsfs_alloc.h"

#include "slsfs_subr.h"
#include "slsfs.h"
#include "slsfs_dir.h"
#include "slsfs_buf.h"
#include "slsfs_alloc.h"


static MALLOC_DEFINE(M_SLSFSMNT, "slsfs_mount", "SLS mount structures");

static vfs_root_t	slsfs_root;
static vfs_statfs_t	slsfs_statfs;
static vfs_vget_t	slsfs_vget;
static vfs_sync_t	slsfs_sync;

extern struct buf_ops bufops_slsfs;

static const char *slsfs_opts[] = { "from" };
struct unrhdr *slsid_unr;

struct slsfs_taskctx {
	struct task tk;
	struct vnode *vp;
	struct vm_object *vmobj;
	slsfs_callback cb;
	vm_page_t startpage;
	int iotype;
	uint64_t size;
};

struct extent {
	uint64_t start;
	uint64_t end;
	uint64_t target;
};

static void
extent_clip_head(struct extent *extent, uint64_t boundary)
{
	if (boundary < extent->start) {
		extent->start = boundary;
	}
	if (boundary < extent->end) {
		extent->end = boundary;
	}
}

static void
extent_clip_tail(struct extent *extent, uint64_t boundary)
{
	if (boundary > extent->start) {
		if (extent->target != 0) {
			extent->target += boundary - extent->start;
		}
		extent->start = boundary;
	}
	if (boundary > extent->end) {
		extent->end = boundary;
	}
}

static void
set_extent(struct extent *extent, uint64_t lbn, uint64_t size, uint64_t target)
{
	KASSERT(size % PAGE_SIZE == 0, ("Size %lu is not a multiple of the page size", size));

	extent->start = lbn;
	extent->end = lbn + size / PAGE_SIZE;
	extent->target = target;
}

static void
diskptr_to_extent(struct extent *extent, uint64_t lbn, const diskptr_t *diskptr)
{
	set_extent(extent, lbn, diskptr->size, diskptr->offset);
}

static void
extent_to_diskptr(const struct extent *extent, uint64_t *lbn, diskptr_t *diskptr)
{
	*lbn = extent->start;
	diskptr->offset = extent->target;
	diskptr->size = (extent->end - extent->start) * PAGE_SIZE;
}

/*
 * Insert an extent starting an logical block number lbn of size bytes into the
 * btree, potentially splitting existing extents to make room.
 */
static int
slsfs_fbtree_rangeinsert(struct fbtree *tree, uint64_t lbn, uint64_t size)
{
	/*
	 * We are inserting an extent which may overlap with many other extents
	 * in the tree.  There are many possible scenarios:
	 *
	 *           [----)
	 *     [-------)[-------)
	 *
	 *           [----)
	 *     [----------------)
	 *
	 *           [----)
	 *     [--)          [--)
	 *
	 * etc.  We want to end up with
	 *
	 *            main
	 *     [----)[----)[----)
	 *      head        tail
	 *
	 * (with head and/or tail possibly empty).
	 *
	 * To do this, we iterate over every extent (current) that may possibly
	 * intersect the new one (main).  For each of these, we clip that extent
	 * into a possible new_head and new_tail.  There will be at most one
	 * head and one tail that are non-empty.  We remove all the overlapping
	 * extents, then insert the new head, main, and tail extents.
	 */

	struct extent main;
	struct extent head = {0}, tail = {0};
	struct extent current;
	struct extent new_head, new_tail;
	struct fnode_iter iter;
	int error;
	uint64_t key;
	diskptr_t value;

	set_extent(&main, lbn, size, 0);

	key = main.start;
	error = fbtree_keymin_iter(tree, &key, &iter);
	if (error) {
		panic("fbtree_keymin_iter() error %d in range insert", error);
	}

	if (ITER_ISNULL(iter)) {
		// No extents are before the start, so start from the beginning
		iter.it_index = 0;
	}

	while (!ITER_ISNULL(iter)) {
		key = ITER_KEY_T(iter, uint64_t);
		value = ITER_VAL_T(iter, diskptr_t);

		diskptr_to_extent(&current, key, &value);
		if (current.start >= main.end) {
			break;
		}

		new_head = current;
		extent_clip_head(&new_head, main.start);
		if (new_head.start != new_head.end) {
			KASSERT(head.start == head.end,
			        ("Found multiple heads [%lu, %lu) and [%lu, %lu)",
			         head.start, head.end, new_head.start, new_head.end));
			head = new_head;
		}

		new_tail = current;
		extent_clip_tail(&new_tail, main.end);
		if (new_tail.start != new_head.end) {
			KASSERT(tail.start == tail.end,
			        ("Found multiple tails [%lu, %lu) and [%lu, %lu)",
			         tail.start, tail.end, new_tail.start, new_tail.end));
			tail = new_tail;
		}

		error = fiter_remove(&iter);
		if (FBTREE_ERROR(error)) {
			panic("Error %d removing current", error);
		}
	}

	if (head.start != head.end) {
		extent_to_diskptr(&head, &key, &value);
		error = fbtree_insert(tree, &key, &value);
		if (FBTREE_ERROR(error)) {
			panic("Error %d inserting head", error);
		}
	}

	extent_to_diskptr(&main, &key, &value);
	error = fbtree_insert(tree, &key, &value);
	if (FBTREE_ERROR(error)) {
		panic("Error %d inserting main", error);
	}

	if (tail.start != tail.end) {
		extent_to_diskptr(&tail, &key, &value);
		error = fbtree_insert(tree, &key, &value);
		if (FBTREE_ERROR(error)) {
			panic("Error %d inserting tail", error);
		}
	}

	return (0);
}

/*
 * Register the Aurora filesystem type with the kernel.
 */
static int
slsfs_init(struct vfsconf *vfsp)
{
	/* Setup slos structures */
	slos_init();

	/* Get a new unique identifier generator. */
	slsid_unr = new_unrhdr(SLOS_SYSTEM_MAX, INT_MAX, NULL);
	fnode_zone = uma_zcreate("Btree Fnode slabs", sizeof(struct fnode), NULL, NULL, NULL, NULL, UMA_ALIGN_PTR, 0);

	/* The constructor never fails. */
	KASSERT(slsid_unr != NULL, ("slsid unr creation failed"));;
	return (0);
}

/*
 * Unregister the Aurora filesystem type from the kernel.
 */
static int
slsfs_uninit(struct vfsconf *vfsp)
{
	/* Destroy the identifier generator. */
	clean_unrhdr(slsid_unr);
	clear_unrhdr(slsid_unr);
	delete_unrhdr(slsid_unr);
	slsid_unr = NULL;
	uma_zdestroy(fnode_zone);
	slos_uninit();

	return (0);
}

/*
 * Bring the inode that holds the rest of the filesystem inodes into memory.
 */
static int
slsfs_inodes_init(struct mount *mp, struct slos *slos)
{
	int error;

	/* Create the vnode for the inode root. */
	error = slsfs_vget(mp, SLOS_INODES_ROOT, 0, &slos->slsfs_inodes);
	if (error) {
		return (error);
	}

	/* Create the filesystem root. */
	DBUG("Initing the root inode\n");
	error = slos_icreate(slos, SLOS_ROOT_INODE, MAKEIMODE(VDIR, S_IRWXU));
	if (error == EINVAL) {
		DBUG("Already exists\n");
	} else if (error) {
		return (error);
	}

	return (0);
}

static void
slsfs_bdone(struct buf *bp)
{
	/* Free the reference to the object taken at the beginning of the IO. */
	vm_object_deallocate(bp->b_pages[0]->object);
	bdone(bp);
}

/* Perform an IO without copying from the VM objects to the buffer. */
static void
slsfs_performio(void *ctx, int pending)
{
	vm_page_t start;
	struct buf *bp;
	size_t bytecount;
	struct slsfs_taskctx *task = (struct slsfs_taskctx *)ctx;
	struct slos_node *svp = SLSVP(task->vp);
	struct slos_inode *sivp = &SLSINO(svp);
	struct fbtree *tree = &svp->sn_tree;
	int iotype = task->iotype;
	vm_pindex_t size; 
	size_t offset;
	int i;

	KASSERT(iotype == BIO_READ || iotype == BIO_WRITE, ("invalid IO type %d", iotype));
	/* 
	 * We can do the merging of keys and all the functionality here.  I'm 
	 * sure this code could be cleaned up but for readability separating all 
	 * the cases pretty distinctly.
	 *
	 * This is only needed for writes, reads are just a bio.
	 */
	if (iotype == BIO_WRITE) {
		BTREE_LOCK(tree, LK_EXCLUSIVE);
		if (task->tk.ta_func == NULL)
			DBUG("Warning: %s called synchronously\n", __func__);

		slsfs_fbtree_rangeinsert(tree, task->startpage->pindex + 1, task->size);
		BTREE_UNLOCK(tree, 0);

	}

	bp = malloc(sizeof(struct buf), M_SLOS, M_WAITOK);
	size = 0;
	start = task->startpage;

	i = 0;
	TAILQ_FOREACH_FROM(start, &task->vmobj->memq, listq) {
		// We are done grabbing pages;
		bp->b_pages[i] = start;
		size += pagesizes[start->psind];
		if (size == task->size) {
			break;
		}
		i++;
	}

	/* Now that the btree is edited we must create the buffer to perform the 
	 * write
	 */

	bp->b_npages = i + 1;
	bp->b_pgbefore = 0;
	bp->b_pgafter = 0;
	bp->b_offset = 0;
	bp->b_data = unmapped_buf;
	bp->b_offset = 0;

	bytecount = bp->b_npages << PAGE_SHIFT;
	offset = IDX_TO_OFF(task->startpage->pindex + 1);
	if (offset + bytecount > sivp->ino_size) {
		sivp->ino_size = offset + bytecount;
		vnode_pager_setsize(task->vp, sivp->ino_size);
	}

	bp->b_vp = task->vp;
	bp->b_bufobj = &task->vp->v_bufobj;
	bufobj_wref(bp->b_bufobj);
	bp->b_iocmd = iotype;
	bp->b_rcred = crhold(curthread->td_ucred);
	bp->b_wcred = crhold(curthread->td_ucred);
	bp->b_bcount = bp->b_bufsize = bp->b_runningbufspace = bytecount;
	atomic_add_long(&runningbufspace, bp->b_runningbufspace);
	bp->b_iodone = slsfs_bdone;
	bp->b_lblkno = task->startpage->pindex + 1;
	bp->b_flags = B_MANAGED;

	VOP_LOCK(task->vp, LK_SHARED);
	bstrategy(bp);
	VOP_UNLOCK(task->vp, LK_SHARED);
	bwait(bp, PVM, "slsfs vm wait");

	KASSERT(bp->b_npages > 0, ("no pages in the IO"));
	for (int i = 0; i < bp->b_npages; i++) {
		bp->b_pages[i] = NULL;
	}

	bp->b_vp = NULL;
	bp->b_bufobj = NULL;
	free(task, M_SLOS);
	free(bp, M_SLOS);
}

static struct slsfs_taskctx *
slsfs_createtask(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len, int iotype, slsfs_callback cb)
{
	struct slsfs_taskctx *ctx;

	ctx = malloc(sizeof(*ctx), M_SLOS, M_WAITOK | M_ZERO);
	*ctx = (struct slsfs_taskctx) {
		.vp = vp,
		.vmobj = obj,
		.startpage = m,
		.size = len,
		.iotype = iotype,
		.cb = cb,
	};

	return (ctx);
}

static int
slsfs_io_async(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len, int iotype, slsfs_callback cb)
{
	struct slos *slos = SLSVP(vp)->sn_slos;
	struct slsfs_taskctx *ctx;

	/* Ignore IOs of size 0. */
	if (len == 0)
		return (0);

	ctx = slsfs_createtask(vp, obj, m, len, iotype, cb);
	TASK_INIT(&ctx->tk, 0, &slsfs_performio, ctx);
	DBUG("Creating task for vnode %p: page index %lu, size %lu\n", vp, m->pindex, len);
	//KASSERT(len != 0, ("IO of size 0"));
	vm_object_reference(m->object);
	taskqueue_enqueue(slos->slos_tq, &ctx->tk);

	return (0);
}

static int
slsfs_io(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len, int iotype)
{
	struct slsfs_taskctx *ctx;

	/* Ignore IOs of size 0. */
	if (len == 0)
		return (0);

	ctx = slsfs_createtask(vp, obj, m, len, iotype, NULL);
	KASSERT(len != 0, ("IO of size 0"));
	vm_object_reference(m->object);
	slsfs_performio(ctx, 0);

	return (0);
}

/*
 * Create an in-memory representation of the SLOS.
 */
static int
slsfs_create_slos(struct mount *mp, struct vnode *devvp)
{
	int error;

	slos.slos_vp = devvp;

	/* Hook up the SLOS into the GEOM provider for the backing device. */
	g_topology_lock();
	error = g_vfs_open(devvp, &slos.slos_cp, "slsfs", 1);
	if (error) {
		printf("Error in opening GEOM vfs\n");
		return error;
	}
	slos.slos_pp = g_dev_getprovider(devvp->v_rdev);
	g_topology_unlock();

	/* Read in the superblock. */
	DBUG("SLOS Read in Super\n");
	error = slos_sbread(&slos);
	if (error != 0) {
		DBUG("ERROR: slos_sbread failed with %d\n", error);
		printf("Problem with slos\n");
		if (slos.slos_cp != NULL) {
			g_topology_lock();
			g_vfs_close(slos.slos_cp);
			dev_ref(slos.slos_vp->v_rdev);
			g_topology_unlock();
		}
		return error;
	}

	lockinit(&slos.slos_lock, PVFS, "sloslock", VLKTIMEOUT, LK_NOSHARE);
	cv_init(&slos.slsfs_sync_cv, "SLSFS Syncer CV");
	mtx_init(&slos.slsfs_sync_lk, "syncer lock",  NULL, MTX_DEF);
	slos.slsfs_dirtybufcnt = 0;
	slos.slsfs_syncing = 0;
	slos.slsfs_checkpointtime = 1;
	slos.slsfs_mount = mp;

	slos.slsfs_io = &slsfs_io;
	slos.slsfs_io_async = &slsfs_io_async;
	DBUG("Creating taskqueue\n");
	slos.slos_tq = NULL;
	slos.slos_tq = taskqueue_create("SLOS Taskqueue", M_WAITOK, taskqueue_thread_enqueue, &slos.slos_tq);
	if (slos.slos_tq == NULL) {
		panic("Problem creating taskqueue\n");
	}

	devvp->v_data = &slos;

	/* Initialize in memory the allocator and the vnode used for inode 
	 * bookkeeping. */
	slsfs_allocator_init(&slos);
	slsfs_inodes_init(mp, &slos);
	VOP_UNLOCK(slos.slos_vp, 0);

	// Start the threads, probably should have a sysctl to define number of 
	// threads here.
	error = taskqueue_start_threads(&slos.slos_tq, 10, PVM, "SLOS Taskqueue Threads");
	if (error) {
		panic("%d issue starting taskqueue\n", error);
	}
	DBUG("SLOS Loaded.\n");
	return error;
}

/*
 * Mount a device as a SLOS, and create an in-memory representation.
 */
static int
slsfs_mount_device(struct vnode *devvp, struct mount *mp, struct slsfs_device **slsfsdev)
{
	struct slsfs_device *sdev;
	void *vdata;
	int error;

	/*
	 * Get a pointer to the device vnode's current private data.
	 * We need to restore it when destroying the SLOS. The field
	 * is overwritten by slsfs_create_slos.
	 */
	vdata = devvp->v_data;

	/* Create the in-memory SLOS. */
	error = slsfs_create_slos(mp, devvp);
	if (error != 0) {
		printf("Error creating SLOS - %d\n", error);
		return (error);
	}


	/*
	 * XXX Is the slsfs_device abstraction future-proofing for when we have an
	 * N to M corresponding between SLOSes and devices? Why isn't the information
	 * in the SLOS enough?
	 */
	sdev = malloc(sizeof(struct slsfs_device), M_SLSFSMNT, M_WAITOK | M_ZERO);
	sdev->refcnt = 1;
	sdev->devvp = slos.slos_vp;
	sdev->gprovider = slos.slos_pp;
	sdev->gconsumer = slos.slos_cp;
	sdev->devsize = slos.slos_pp->mediasize;
	sdev->devblocksize = slos.slos_pp->sectorsize;
	sdev->vdata = vdata;
	mtx_init(&sdev->g_mtx, "slsfsmtx", NULL, MTX_DEF);

	DBUG("Mounting Device Done\n");
	*slsfsdev = sdev;

	return (0);
}
/*
 * Vnode allocation
 * Will create the underlying inode as well and link the data to a newly
 * allocated vnode.  Does not link the allocated vnode to its parent. The
 * parent is passed in as a structure that gets us the mount point as well as
 * the slos.
 *
 * The mount point is needed to allow us to attach the vnode to it, and slos is
 * needed for access the device.
 */
static int
slsfs_valloc(struct vnode *dvp, mode_t mode, struct ucred *creds, struct vnode **vpp)
{
	int error;
	uint64_t pid = 0;

	/* Create the new inode in the filesystem. */
	error = slsfs_new_node(VPSLOS(dvp), mode, &pid);
	if (error) {
		return (error);
	}

	/* Get a vnode for the newly created inode. */
	error = slsfs_vget(dvp->v_mount, pid, LK_EXCLUSIVE, vpp);
	if (error) {
		return (error);
	}

	return (0);
}

/*
 * Mount the filesystem in the device backing the vnode to the mountpoint.
 */
static int
slsfs_mountfs(struct vnode *devvp, struct mount *mp)
{
	struct slsfsmount *smp = NULL;
	struct slsfs_device *slsfsdev = NULL;
	int error, ronly;
	char * from;

	ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

	/* Configure the filesystem to have the IO parameters of the device. */
	if (devvp->v_rdev->si_iosize_max != 0)
		mp->mnt_iosize_max = devvp->v_rdev->si_iosize_max;

	if (mp->mnt_iosize_max > MAXPHYS)
		mp->mnt_iosize_max = MAXPHYS;

	from = vfs_getopts(mp->mnt_optnew, "from", &error);
	if (error)
		goto error;

	/* Create the in-memory data for the filesystem instance. */
	smp = (struct slsfsmount *)malloc(sizeof(struct slsfsmount), M_SLSFSMNT, M_WAITOK | M_ZERO);
	if (error) {
		goto error;
	}

	smp->sp_vfs_mount = mp;
	smp->sp_ronly = ronly;
	smp->sls_valloc = &slsfs_valloc;
	smp->sp_slos = &slos;

	CTR1(KTR_SPARE5, "slsfs_mountfs(%p)", devvp);
	/* Create the in-memory data for the backing device. */
	error = slsfs_mount_device(devvp, mp, &slsfsdev);
	if (error) {
		goto error;
	}

	smp->sp_sdev = slsfsdev;

	KASSERT(smp->sp_slos != NULL, ("Null slos"));
	if (error)
		goto error;

	mp->mnt_data = smp;

	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_USES_BCACHE;
	MNT_IUNLOCK(mp);

#ifdef SLOS_TEST

	printf("Testing fbtreecomponent...\n");
	error = slsfs_fbtree_test();
	if (error != 0)
		printf("ERROR: Test failed with %d\n", error);

	/* XXX Returning an error locks up the mounting process. */

#endif /* SLOS_TEST */

	return (0);
error:
	if (smp != NULL) {
		free(smp, M_SLSFSMNT);
		mp->mnt_data = NULL;
	}

	if (slsfsdev != NULL) {
		free(slsfsdev, M_SLSFSMNT);
	}

	printf("Error mounting");
	vput(devvp);

	return (error);
}

/*
 * Flush all dirty buffers related to the SLOS to the backend.
 */
static void
slsfs_checkpoint(struct mount *mp, int closing)
{
	struct vnode *vp, *mvp;
	struct thread *td;
	struct bufobj *bo;
	int error, dirty;

	td = curthread;
	/* Go throught the list of vnodes attached to the filesystem. */
	MNT_VNODE_FOREACH_ACTIVE(vp, mp, mvp) {
		/* XXX Is this right? Why are we unlocking the vnode's VI lock? We never locked it.  */
		if(VOP_ISLOCKED(vp)) {
			VI_UNLOCK(vp);
			continue;
		}

		/* If we can't get a reference, the vnode is probably dead. */
		if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK | LK_NOWAIT, td) != 0) {
			continue;
		}

		bo = &vp->v_bufobj;
		// XXX We cant let the slsfs_dev sync normally as its a fake inode
		// and doesnt have all the interconects a regular one does. We
		// may one to flush it out and just make it easier for the
		// abstraction, meaning make a fake inode and fake slos_node
		// for it as well.
		/* Only sync the vnode if it has been dirtied. */
		/* XXX Why return on an error? We should keep going with the 
		 * rest. */
		if ((vp->v_vflag & VV_SYSTEM) || ((vp->v_type != VDIR) && (vp->v_type != VREG))) {
			vput(vp);
			continue;
		}
		if (SLSVP(vp)->sn_status & SLOS_DIRTY) {
			dirty = slsfs_sync_vp(vp, closing);
			if (dirty == -1) {
				vput(vp);
				return;
			}

			/* Update the inode root with the inode's new information. */
			DBUG("Updating root\n");
			SLSVP(vp)->sn_status &= ~(SLOS_DIRTY);
			error = slos_updateroot(SLSVP(vp));
			if (error) {
				vput(vp);
				return;
			}
		}
		vput(vp);
	}

	// Just a hack for now to get this thing working XXX Why is it a hack?
	/* Sync the inode root itself. */
	VOP_LOCK(slos.slsfs_inodes, LK_EXCLUSIVE);
	error = slsfs_sync_vp(slos.slsfs_inodes, closing);
	if (error) {
		return;
	}
	VOP_UNLOCK(slos.slsfs_inodes, 0);

	// Just a hack for now to get this thing working XXX Same
	/* Sync any raw device buffers. */
}

/*
 * Daemon that flushes dirty buffers to the device.
 *
 * XXX It's very interesting how this combines with
 * explicit checkpoints
 */
static void
slsfs_syncer(struct slos *slos)
{
	slos->slsfs_sync_exit = 0;

	/* Periodically sync until we unmount. */
	while (!slos->slsfs_sync_exit) {
		slsfs_checkpoint(slos->slsfs_mount, 0);

		/* Notify anyone waiting to synchronize. */
		mtx_lock(&slos->slsfs_sync_lk);
		slos->slsfs_syncing = 0;
		cv_broadcast(&slos->slsfs_sync_cv);
		mtx_unlock(&slos->slsfs_sync_lk);

		/* Wait until it's time to flush again. */
		if (!slos->slsfs_sync_exit) {
			tsleep(&slos->slsfs_syncing, PRIBIO, "-",
			    (hz * slos->slsfs_checkpointtime) >> 1);
		}
	}

	DBUG("Syncer exiting\n");

	/* One last checkpoint before we exit. */
	slsfs_checkpoint(slos->slsfs_mount, 1);

	/* Notify anyone else waiting to flush one last time. */
	mtx_lock(&slos->slsfs_sync_lk);
	slos->slsfs_syncing = 0;
	cv_broadcast(&slos->slsfs_sync_cv);
	mtx_unlock(&slos->slsfs_sync_lk);

	slos->slsfs_syncertd = NULL;

	kthread_exit();
}

/*
 * Initialize the in-memory state of the filesystem instance.
 */
static int
slsfs_init_fs(struct mount *mp)
{
	struct vnode *vp = NULL;
	int error;

	/* Get the filesystem root and initialize it if this is the first mount. */
	VFS_ROOT(mp, LK_EXCLUSIVE, &vp);
	if (vp == NULL) {
		return (EIO);
	}

	/* Set up the syncer. */
	slos.slsfs_mount = mp;
	error = kthread_add((void(*)(void *))slsfs_syncer, &slos, NULL,
	    &slos.slsfs_syncertd, 0, 0, "slsfs syncer");
	if (error) {
		panic("Syncer could not start");
	}

	/*
	 * I have no idea when this bug occured but right now ls -la doesnt 
	 * show up the first directory that gets created.  Even though with 
	 * debugging on I can see it within the buffer and it shows correctly 
	 * there.
	 *
	 * This is a hack as if i create and remove a directory it works 
	 * perfectly fine.
	 *
	 * XXX: Fix this bug
	 *
	 */
	if (SLSINO(SLSVP(vp)).ino_nlink < 2) {
		struct componentname name;
		struct vattr attr = {};
		char * buf = "hackme";
		name.cn_nameptr = buf;
		name.cn_namelen = 5;
		slsfs_init_dir(vp, vp, NULL);
		struct vnode *svp;
		VOP_MKDIR(vp, &svp, &name, &attr);
		VOP_RMDIR(vp, svp, &name);
		VOP_CLOSE(svp, 0, NULL, NULL);
		vput(svp);
	}

	/* FOR BTREE TESTING
	struct vnode *test;
	SLS_VALLOC(vp, 0, NULL, &test);
	fbtree_test(&SLSVP(test)->sn_tree);
	panic("test done");
	*/
	vput(vp);

	return (0);
}

/*
 * Mount the filesystem.
 */
static int
slsfs_mount(struct mount *mp)
{
	DBUG("Mounting slsfs\n");
	struct vnode *devvp;
	struct nameidata nd;
	struct vfsoptlist *opts;
	int error = 0;
	char *from;

	opts = mp->mnt_optnew;
	vfs_filteropt(opts, slsfs_opts);
	if (mp->mnt_flag & MNT_UPDATE) {
		// TODO: Update of Mount
	}

	/* Get the vnode device to mount from the path. */
	from = vfs_getopts(opts, "from", &error);
	if (error != 0) {
		return (error);
	}

	NDINIT(&nd, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, from, curthread);
	error = namei(&nd);
	if (error) {
		return (error);
	}
	NDFREE(&nd, NDF_ONLY_PNBUF);

	devvp = nd.ni_vp;
	if (!vn_isdisk(devvp, &error)) {
		/* XXX Can we make it so we can use a file? */
		DBUG("Dbug is not a disk\n");
		vput(devvp);
		return (error);
	}

	/* XXX This seems like it's a leftover from before.  */
	if (error) {
		vput(devvp);
		return (error);
	}

	/* Get an ID for the new filesystem. */
	vfs_getnewfsid(mp);

	/* Actually mount the vnode as a filesyste and initialize its state. */
	error = slsfs_mountfs(devvp, mp);
	if (error) {
		return (error);
	}

	error = slsfs_init_fs(mp);
	if (error) {
		return (error);
	}

	/* Get the path where we found the device. */
	vfs_mountedfrom(mp, from);
	return (0);
}

/*
 *  Return the vnode for the root of the filesystem.
 */
static int
slsfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	/*
	 * Use the already implemented VFS method with the
	 * hardcoded value for the filesystem root.
	 */
	error = VFS_VGET(mp, SLOS_ROOT_INODE, flags, &vp);
	if (error)
		return (error);

	vp->v_type = VDIR;
	*vpp = vp;
	return(0);
}

/*
 * Export stats for the filesystem.
 * XXX Implement eventually
 */
static int
slsfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct slsfs_device	    *slsdev;
	struct slsfsmount	    *smp;

	smp = TOSMP(mp);
	slsdev = smp->sp_sdev;
	sbp->f_bsize = slsdev->devblocksize;
	sbp->f_iosize = sbp->f_bsize;
	sbp->f_blocks = 1000;
	sbp->f_bfree = 100;
	sbp->f_bavail = 100;
	sbp->f_ffree = 0;
	sbp->f_files = 1000;

	return (0);
}

/*
 * Unmount a device from the SLOS system and the kernel.
 */
static int
slsfs_unmount_device(struct slsfs_device *sdev)
{
	int error = 0;

	SLOS_LOCK(&slos);

	/* Unhook the SLOS from the GEOM layer. */
	if (slos.slos_cp != NULL) {
		g_topology_lock();
		g_vfs_close(slos.slos_cp);
		dev_ref(slos.slos_vp->v_rdev);
		g_topology_unlock();
	}

	SLOS_UNLOCK(&slos);

	slsfs_allocator_uninit(&slos);

	/* Destroy related in-memory locks. */
	lockdestroy(&slos.slos_lock);

	free(slos.slos_sb, M_SLOS_SB);

	/* Destroy the device. */
	mtx_destroy(&sdev->g_mtx);

	/* Restore the node's private data. */
	sdev->devvp->v_data = sdev->vdata;

	free(sdev, M_SLSFSMNT);

	DBUG("Device Unmounted\n");

	return error;
}

/*
 * Destroy the mounted filesystem data.
 */
static void
slsfs_freemntinfo(struct mount *mp)
{
	DBUG("Destroying slsmount info\n");
	struct slsfsmount *smp = TOSMP(mp);

	if (mp == NULL)
		return;

	free(smp, M_SLSFSMNT);
	DBUG("Destroyed slsmount info\n");
}

/*
 * Wake up the SLOS syncer.
 */
static int
slsfs_wakeup_syncer(int is_exiting)
{
	mtx_lock(&slos.slsfs_sync_lk);
	/* Don't sync again if already in progress. */
	/* XXX Maybe exit if it's already in progress? How do we
	 * serialize writes and syncs? (If a write after the last
	 * sync but before this one doesn't get  written by the former
	 * then we have to go through with the latter).
	 */
	if (slos.slsfs_syncing) {
		DBUG("Wait\n");
		cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	}

	slos.slsfs_syncing = 1;
	if (is_exiting) {
		slos.slsfs_sync_exit = 1;
	}

	/* The actual wakeup. */
	wakeup(&slos.slsfs_syncing);

	/* Wait until the syncer notifies us it's done. */
	cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	mtx_unlock(&slos.slsfs_sync_lk);

	return (0);
}

/*
 * Unmount the filesystem from the kernel.
 */
static int
slsfs_unmount(struct mount *mp, int mntflags)
{
	struct slsfs_device *sdev;
	struct slsfsmount *smp;
	struct slos *slos;
	int error;
	int flags = 0;

	DBUG("UNMOUNTING\n");
	smp = mp->mnt_data;
	sdev = smp->sp_sdev;
	slos = smp->sp_slos;
	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
	}

	/* Free the slos taskqueue */
	taskqueue_free(slos->slos_tq);
	slos->slos_tq = NULL;

	/* Remove all SLOS_related vnodes. */
	error = vflush(mp, 0, flags/* | SKIPSYSTEM*/, curthread);
	if (error) {
		return (error);
	}
	/*
	 * Flush the data to the disk. We have already removed all
	 * vnodes, so this is going to be the last flush we need.
	 */
	slsfs_wakeup_syncer(1);

	/* 
	 * Seems like we don't call reclaim on a reference count drop so I 
	 * manually call slos_vpfree to release the memory.
	 */
	slos_vpfree(slos, SLSVP(slos->slsfs_inodes));
	vrele(slos->slsfs_inodes);
	slos->slsfs_inodes = NULL;

	DBUG("Flushed all active vnodes\n");
	/* Remove the mounted device. */
	error = slsfs_unmount_device(sdev);
	if (error) {
		return (error);
	}

	cv_destroy(&slos->slsfs_sync_cv);
	mtx_destroy(&slos->slsfs_sync_lk);
	slsfs_freemntinfo(mp);

	DBUG("Freeing mount info\n");
	mp->mnt_data = NULL;
	DBUG("Changing mount flags\n");


	/* We've removed the local filesystem info. */
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	return (0);
}

/*
 *
 */
static void
slsfs_init_vnode(struct vnode *vp, uint64_t ino)
{
	struct slos_node *mp = SLSVP(vp);
	if (ino == SLOS_ROOT_INODE) {
		vp->v_vflag |= VV_ROOT;
	} else if (ino == SLOS_INODES_ROOT) {
		vp->v_type = VREG;
	} else {
		vp->v_type = IFTOVT(mp->sn_ino.ino_mode);
	}
}

/*
 * Get a new vnode for the specified SLOS inode.
 */
static int
slsfs_vget(struct mount *mp, uint64_t ino, int flags, struct vnode **vpp)
{
	int error;
	struct vnode *vp;
	struct vnode *none;
	struct slos_node * svnode;
	struct thread *td;

	td = curthread;
	vp = NULL;
	none = NULL;

	/* Make sure the inode does not already have a vnode. */
	error = vfs_hash_get(mp, ino, LK_EXCLUSIVE, td, &vp,
	    NULL, NULL);
	if (error) {
		return (error);
	}

	/* If we do have a vnode already, return it. */
	if (vp != NULL) {
		*vpp = vp;
		return (0);
	}

	/* Bring the inode in memory. */
	error = slsfs_get_node(&slos, ino, &svnode);
	if (error) {
		*vpp = NULL;
		return (error);
	}

	/* Get a new blank vnode. */
	error = getnewvnode("slsfs", mp, &sls_vnodeops, &vp);
	if (error) {
		DBUG("Problem getting new inode\n");
		*vpp = NULL;
		return (error);
	}

	/*
	 * If the vnode is not the root, which is managed directly
	 * by the SLOS, add it to the mountpoint.
	 */
	if (ino != SLOS_INODES_ROOT) {
		lockmgr(&vp->v_lock, LK_EXCLUSIVE, NULL);
		error = insmntque(vp, mp);
		if (error) {
			DBUG("Problem queing root into mount point\n");
			*vpp = NULL;
			return (error);
		}
	}

	svnode->sn_slos = &slos;
	vp->v_data = svnode;
	vp->v_bufobj.bo_ops = &bufops_slsfs;
	vp->v_bufobj.bo_bsize = IOSIZE(svnode);
	slsfs_init_vnode(vp, ino);

	/* Again, if we're not the inode metanode, add bookkeeping. */
	/*
	 * XXX Why? This means we would get a different vnode every time
	 * we try to open the metanode.
	 */
	if (ino != SLOS_INODES_ROOT) {
		error = vfs_hash_insert(vp, ino, 0, td, &none, NULL, NULL);
		if (error || none != NULL) {
			DBUG("Problem with vfs hash insert\n");
			*vpp = NULL;
			return (error);
		}
	}
	DBUG("vget(%p) ino = %ld\n", vp, ino);
	*vpp = vp;
	return (0);
}


/*
 * Wakeup the syncer for a regular sync.
 */
static int
slsfs_sync(struct mount *mp, int waitfor)
{
	slsfs_wakeup_syncer(0);
	return (0);
}

static struct vfsops slsfs_vfsops = {
	.vfs_init =		slsfs_init,
	.vfs_uninit =		slsfs_uninit,
	.vfs_root =		slsfs_root,
	.vfs_statfs =		slsfs_statfs,
	.vfs_mount =		slsfs_mount,
	.vfs_unmount =		slsfs_unmount,
	.vfs_vget =		slsfs_vget,
	.vfs_sync =		slsfs_sync
};

VFS_SET(slsfs_vfsops, slsfs, 0);
MODULE_VERSION(slsfs, 0);
