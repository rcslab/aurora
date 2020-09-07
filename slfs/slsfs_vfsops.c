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
#include <sys/sysctl.h>
#include <sys/syscallsubr.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_page.h>

#include <slos.h>
#include <slos_inode.h>
#include <slos_btree.h>
#include <slos_io.h>
#include <slosmm.h>
#include <btree.h>
#include <slsfs.h>

#include "slos_alloc.h"
#include "slos_subr.h"
#include "slsfs_dir.h"
#include "slsfs_buf.h"
#include "debug.h"

static MALLOC_DEFINE(M_SLSFSBUF, "slsfs_buf", "SLSFS buf");
static MALLOC_DEFINE(M_SLSFSMNT, "slsfs_mount", "SLSFS mount structures");
static MALLOC_DEFINE(M_SLSFSTSK, "slsfs_taskctx", "SLSFS task context");

static vfs_root_t	slsfs_root;
static vfs_statfs_t	slsfs_statfs;
static vfs_vget_t	slsfs_vget;
static vfs_sync_t	slsfs_sync;

extern struct buf_ops bufops_slsfs;

static const char *slsfs_opts[] = { "from" };
struct unrhdr *slsid_unr;

// sysctl variables
int checksum_enabled = 0;

struct slsfs_taskctx {
	struct task tk;
	struct vnode *vp;
	vm_object_t obj;
	slsfs_callback cb;
	vm_page_t startpage;
	int iotype;
	uint64_t size;
};

struct slsfs_taskbdctx {
	struct task tk;
	struct buf *bp;
};

struct extent {
	uint64_t start;
	uint64_t end;
	uint64_t target;
	uint64_t epoch;
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
set_extent(struct extent *extent, uint64_t lbn, uint64_t size, uint64_t target, uint64_t epoch)
{
	KASSERT(size % PAGE_SIZE == 0, ("Size %lu is not a multiple of the page size", size));

	extent->start = lbn;
	extent->end = lbn + (size / PAGE_SIZE);
	extent->target = target;
	extent->epoch = epoch;
}

static void
diskptr_to_extent(struct extent *extent, uint64_t lbn, const diskptr_t *diskptr)
{
	set_extent(extent, lbn, diskptr->size, diskptr->offset, diskptr->epoch);
}

static void
extent_to_diskptr(const struct extent *extent, uint64_t *lbn, diskptr_t *diskptr)
{
	*lbn = extent->start;
	diskptr->offset = extent->target;
	diskptr->size = (extent->end - extent->start) * PAGE_SIZE;
	diskptr->epoch = extent->epoch;
}

/*
 * Insert an extent starting an logical block number lbn of size bytes into the
 * btree, potentially splitting existing extents to make room.
 */
int
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

#ifdef VERBOSE
	DEBUG5("%s:%d: %s with logical offset %u, len %u", __FILE__, __LINE__, __func__, lbn, size);
#endif
	set_extent(&main, lbn, size, 0, 0);
#ifdef VERBOSE
	DEBUG5("%s:%d: %s with extent [%u, %u)", __FILE__, __LINE__, __func__, main.start, main.end);
#endif

	key = main.start;
	error = fbtree_keymin_iter(tree, &key, &iter);
	if (error) {
		panic("fbtree_keymin_iter() error %d in range insert", error);
	}


	if (ITER_ISNULL(iter)) {
		// No extents are before the start, so start from the beginning
		iter.it_index = 0;
	}

	KASSERT(key <= main.start, ("Got minimum %ld as an infimum for %ld\n", key, main.start));

	while (!ITER_ISNULL(iter) && NODE_SIZE(iter.it_node) > 0 ) {
		key = ITER_KEY_T(iter, uint64_t);
		value = ITER_VAL_T(iter, diskptr_t);

		diskptr_to_extent(&current, key, &value);
#ifdef VERBOSE
		DEBUG4("current [%d, %d), main [%d, %d)", current.start, current.end,
		   main.start, main.end);
#endif
		if (current.start >= main.end) {
#ifdef VERBOSE
			DEBUG("BREAK");
#endif
			break;
		}

		new_head = current;
		extent_clip_head(&new_head, main.start);
		if (new_head.start != new_head.end) {
			KASSERT(head.start == head.end, ("Found multiple heads [%lu, %lu) and [%lu, %lu)",
			         head.start, head.end, new_head.start, new_head.end));
			head = new_head;
#ifdef VERBOSE
			DEBUG2("Head clip [%d, %d)", head.start, head.end);
#endif
		}

		new_tail = current;
		extent_clip_tail(&new_tail, main.end);
		if (new_tail.start != new_tail.end) {
			if (tail.start != tail.end) {
				fnode_print(iter.it_node);
			        panic("Found multiple tails [%lu, %lu) and [%lu, %lu) %lu, %lu",
			         tail.start, tail.end, new_tail.start, new_tail.end, main.start, main.end);
			}
			tail = new_tail;
#ifdef VERBOSE
			DEBUG3("Tail clip [%d, %d), current.start vs main.end %d",
			    tail.start, tail.end, current.start == main.end);
#endif
		}

		error = fiter_remove(&iter);
		if (error) {
			panic("Error %d removing current", error);
		}
	}

	if (head.start != head.end) {
		extent_to_diskptr(&head, &key, &value);
#ifdef VERBOSE
		DEBUG5("(%p, %d): Inserting (%ld, %ld, %ld)", curthread, __LINE__,
			(uint64_t) key, value.offset, value.size);
#endif
		error = fbtree_insert(tree, &key, &value);
		if (error) {
			panic("Error %d inserting head", error);
		}
	}

	extent_to_diskptr(&main, &key, &value);
#ifdef VERBOSE
	DEBUG5("(%p, %d): Inserting (%ld, %ld, %ld)", curthread, __LINE__,
		(uint64_t) key, value.offset, value.size);
#endif
	error = fbtree_insert(tree, &key, &value);
	if (error) {
		panic("Error %d inserting main", error);
	}
	size -= value.size;

	if (tail.start != tail.end) {
		extent_to_diskptr(&tail, &key, &value);
#ifdef VERBOSE
		DEBUG5("(%p, %d): Inserting (%ld, %ld, %ld)", curthread, __LINE__,
		    (uint64_t) key, value.offset, value.size);
#endif
		error = fbtree_insert(tree, &key, &value);
		if (error) {
			fnode_print(iter.it_node);
			panic("Error %d inserting  - %lu : %lu-%lu - %lu-%lu %lu-%lu %lu-%lu", 
				error, key, value.offset, value.size, lbn, size, tail.start, 
				tail.end, head.start, head.end);
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

	fnode_zone = uma_zcreate("Btree Fnode Slabs", sizeof(struct fnode),
		&fnode_construct, &fnode_deconstruct, NULL, NULL, UMA_ALIGN_PTR, 0);
	fnode_trie_zone = uma_zcreate("Btree Fnode Trie Slabs", pctrie_node_size(),
		NULL, NULL, pctrie_zone_init, NULL, UMA_ALIGN_PTR, UMA_ZONE_NOFREE);

	/* The constructor never fails. */
	KASSERT(slsid_unr != NULL, ("slsid unr creation failed"));

	return (0);
}

/*
 * Unregister the Aurora filesystem type from the kernel.
 */
static int
slsfs_uninit(struct vfsconf *vfsp)
{
	//Destroy the identifier generator. 
	clean_unrhdr(slsid_unr);
	clear_unrhdr(slsid_unr);
	delete_unrhdr(slsid_unr);
	slsid_unr = NULL;
	uma_zdestroy(fnode_zone);
	uma_zdestroy(fnode_trie_zone);
	slos_uninit();

	return (0);
}

/*
 * Called after allocator tree and checksum tree have been inited.
 * This function just checks if the mount is a fresh mount (by checking the 
 * epoch number).
 *
 * Then fetches the vnode.  Since everything else has been initialized we
 * can fetch it normally. We then try and create the root directory of the 
 * filesystem and are fine if it fails (means it already exists).
 */
static int
slsfs_inodes_init(struct mount *mp, struct slos *slos)
{
	int error;

	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		DEBUG("Initializing root inode");
		error = initialize_inode(slos, SLOS_INODES_ROOT, &slos->slos_sb->sb_root);
		MPASS(error == 0);
	}
	/* Create the vnode for the inode root. */
	DEBUG1("Initing the root inode %lu", slos->slos_sb->sb_root.offset);
	error = slsfs_vget(mp, SLOS_INODES_ROOT, 0, &slos->slsfs_inodes);
	if (error) {
		panic("Issue trying to find root node on init");
		return (error);
	}
	VOP_UNLOCK(slos->slsfs_inodes, 0);

	/* Create the filesystem root. */
	error = slos_icreate(slos, SLOS_ROOT_INODE, MAKEIMODE(VDIR, S_IRWXU));
	if (error == EINVAL) {
		DEBUG("Already exists");
	} else if (error) {
		return (error);
	}

	return (0);
}

/*
 * Function that turns the IO into a no-op. Cleans up like performio().
 */
static void
slsfs_nullio(void *ctx, int pending)
{
	struct slsfs_taskctx *task = (struct slsfs_taskctx *)ctx;

	KASSERT(task->iotype == BIO_READ || task->iotype == BIO_WRITE,
		("invalid IO type %d", task->iotype));
	ASSERT_VOP_UNLOCKED(task->vp, ("vnode %p is locked", vp));
	KASSERT(IOSIZE(SLSVP(task->vp)) == PAGE_SIZE,  ("IOs are not page-sized (size %lu)", IOSIZE(SLSVP(task->vp))));

	/* Just drop the reference and finish up with the task.*/
	vm_object_deallocate(task->obj);

	free(task, M_SLSFSTSK);
	return;

}

/* Perform an IO without copying from the VM objects to the buffer. */
static void
slsfs_performio(void *ctx, int pending)
{
	struct buf *bp;
	size_t bytecount;
	struct slsfs_taskctx *task = (struct slsfs_taskctx *)ctx;
	struct slos_node *svp = SLSVP(task->vp);
	struct slos_inode *sivp = &SLSINO(svp);
	struct fbtree *tree = &svp->sn_tree;
	int iotype = task->iotype;
	vm_object_t obj = task->obj;
	vm_pindex_t size;
	size_t offset;
	vm_page_t m;
	size_t end;
	int npages;
	int i;

	KASSERT(iotype == BIO_READ || iotype == BIO_WRITE, ("invalid IO type %d", iotype));
	ASSERT_VOP_UNLOCKED(task->vp, ("vnode %p is locked", vp));
	KASSERT(IOSIZE(svp) == PAGE_SIZE,  ("IOs are not page-sized (size %lu)", IOSIZE(svp)));

	bp = malloc(sizeof(*bp), M_SLSFSBUF, M_WAITOK | M_ZERO);
	m = task->startpage;
	npages = 0;
	size = 0;

	DEBUG4("%s:%d: iotype %d for bp %p", __FILE__, __LINE__, iotype, bp);
	KASSERT((task->size / PAGE_SIZE) < btoc(MAXPHYS), ("IO too large"));

	VM_OBJECT_RLOCK(obj);
	TAILQ_FOREACH_FROM(m, &obj->memq, listq) {
		/*  We are done grabbing pages. */
		KASSERT(npages < btoc(MAXPHYS), ("overran b_pages[] array"));
		KASSERT(obj->ref_count >= obj->shadow_count + 1,
		    ("object has %d references and %d shadows",
		    obj->ref_count, obj->shadow_count));
		KASSERT(m->object == obj, ("page %p in object %p "
		    "associated with object %p",
		    m, obj, m->object));
		KASSERT(task->startpage->pindex + npages == m->pindex,
		    ("pages in the buffer are not consecutive "
		    "(pindex should be %ld, is %ld)",
		    task->startpage->pindex + npages, m->pindex));
		bp->b_pages[npages++] = m;
		size += pagesizes[m->psind];
		if (size == task->size) {
			break;
		}
	}
	VM_OBJECT_RUNLOCK(obj);

	/* If the IO would be empty, this function is a no-op.*/
	if (npages == 0) {
		vm_object_deallocate(obj);
		free(bp, M_SLSFSBUF);
		free(task, M_SLSFSTSK);
		return;
	}

	/* The size of the IO in bytes. */
	bytecount = npages << PAGE_SHIFT;

	/*
	 * We can do the merging of keys and all the functionality here.  I'm
	 * sure this code could be cleaned up but for readability separating all
	 * the cases pretty distinctly.
	 *
	 * This is only needed for writes, reads are just a bio.
	 */
	if (iotype == BIO_WRITE) {
		BTREE_LOCK(tree, LK_EXCLUSIVE);
		VOP_LOCK(tree->bt_backend, LK_EXCLUSIVE);

		if (task->tk.ta_func == NULL)
			DEBUG1("Warning: %s called synchronously", __func__);

		slsfs_fbtree_rangeinsert(tree, task->startpage->pindex + SLOS_OBJOFF, task->size);
		end = ((task->startpage->pindex + SLOS_OBJOFF) * IOSIZE(SLSVP(task->vp))) + task->size;
		if (SLSVP(task->vp)->sn_ino.ino_size < end) {
			SLSVP(task->vp)->sn_ino.ino_size = end;
			SLSVP(task->vp)->sn_status |= SLOS_DIRTY;
		}

		DEBUG3("(td %p) vp(%p) - SIZE %lu", curthread, task->vp, SLSVP(task->vp)->sn_ino.ino_size);

		VOP_UNLOCK(tree->bt_backend, 0);
		BTREE_UNLOCK(tree, 0);

		/* Possibly update the size of the file in the VM subsystem. */
		offset = IDX_TO_OFF(task->startpage->pindex + SLOS_OBJOFF);
		DEBUG3("%s:%d: Setting the size of vnode %p", __FILE__, __LINE__, task->vp);
		if (offset + bytecount > sivp->ino_size) {
			sivp->ino_size = offset + bytecount;
			vnode_pager_setsize(task->vp, sivp->ino_size);
		}
	}

	/* Now that the btree is edited we create the buffer for the write. */
	bp->b_npages = npages;
	bp->b_pgbefore = 0;
	bp->b_pgafter = 0;
	bp->b_offset = 0;
	bp->b_data = unmapped_buf;
	bp->b_offset = 0;
	DEBUG4("%s:%d: buf %p has %d pages", __FILE__, __LINE__, bp, bp->b_npages);


	bp->b_vp = task->vp;
	bp->b_bufobj = &task->vp->v_bufobj;
	bp->b_iocmd = iotype;
	bp->b_rcred = crhold(curthread->td_ucred);
	bp->b_wcred = crhold(curthread->td_ucred);
	/* We do not have an associated buffer, we are more akin to a pager. */
	bp->b_runningbufspace = 0;
	atomic_add_long(&runningbufspace, bp->b_runningbufspace);
	bp->b_bcount = bytecount;
	bp->b_bufsize = bytecount;
	bp->b_iodone = bdone;
	bp->b_lblkno = task->startpage->pindex + SLOS_OBJOFF;
	bp->b_flags = B_MANAGED;
	bp->b_vflags = 0;

	KASSERT(bp->b_npages > 0, ("no pages in the IO"));

	DEBUG3("(%p) Getting locks for vnode %p, object %p", curthread, task->vp, bp->b_bufobj);
	/* Update the number of writes in progress if doing a write. */
	VOP_LOCK(task->vp, LK_EXCLUSIVE);
	if (bp->b_iocmd == BIO_WRITE)
		bufobj_wref(bp->b_bufobj);

	bstrategy(bp);
	VOP_UNLOCK(task->vp, 0);

	/*
	 * Wait for the buffer to be done. Because the task calling
	 * slsfs_performio() only exits after bwait() returns, waiting for the
	 * taskqueue to be drained is equivalent to waiting until all data has
	 * hit the disk.
	 */
	bwait(bp, PRIBIO, "slsrw");

	/*
	 * Make sure the buffer is still in a sane state after the IO has
	 * completed.
	 */
	KASSERT(bp->b_npages != 0, ("buffer has no pages"));
	for (i = 0; i < bp->b_npages; i++) {
		KASSERT(bp->b_pages[i]->object != 0, ("page without an associated object found"));
		KASSERT(bp->b_pages[i]->object == obj, ("page %p in object %p "
		    "associated with object %p",
		    bp->b_pages[i], obj, bp->b_pages[i]->object));
	}

	/* Free the reference to the object taken when creating the task.  */
	vm_object_deallocate(bp->b_pages[0]->object);

	free(bp, M_SLSFSBUF);
	free(task, M_SLSFSTSK);
}

static struct slsfs_taskctx *
slsfs_createtask(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len, int iotype, slsfs_callback cb)
{
	struct slsfs_taskctx *ctx;

	ctx = malloc(sizeof(*ctx), M_SLSFSTSK, M_WAITOK | M_ZERO);
	*ctx = (struct slsfs_taskctx) {
		.vp = vp,
		.obj = obj,
		.startpage = m,
		.size = len,
		.iotype = iotype,
		.cb = cb,
	};

	return (ctx);
}

int
slsfs_io_async(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len, int iotype, slsfs_callback cb)
{
	struct slos *slos = SLSVP(vp)->sn_slos;
	struct slsfs_taskctx *ctx;

	/* Ignore IOs of size 0. */
	if (len == 0)
		return (0);

	/*
	 * Get a reference to the object, freed when the buffer is done. We have
	 * to do this here, if we do it in the task the page might be dead by
	 * then.
	 */
	vm_object_reference(obj);

	ctx = slsfs_createtask(vp, obj, m, len, iotype, cb);
	TASK_INIT(&ctx->tk, 0, &slsfs_performio, ctx);
	DEBUG3("Creating task for vnode %p: page index %lu, size %lu", vp, m->pindex, len);
	taskqueue_enqueue(slos->slos_tq, &ctx->tk);

	return (0);
}

int
slsfs_io(struct vnode *vp, vm_object_t obj, vm_page_t m, size_t len, int iotype)
{
	struct slsfs_taskctx *ctx;

	/* Ignore IOs of size 0. */
	if (len == 0)
		return (0);

	/*
	 * Get a reference to the object, freed when the buffer is done. We have
	 * to do this here, if we do it in the task the page might be dead by
	 * then.
	 */
	vm_object_reference(obj);

	ctx = slsfs_createtask(vp, obj, m, len, iotype, NULL);
	KASSERT(len != 0, ("IO of size 0"));
	slsfs_performio(ctx, 0);

	return (0);
}

static int
slsfs_checksumtree_init(struct slos *slos)
{
	diskptr_t ptr;
	int error = 0;

	size_t offset = ((NUMSBS * slos->slos_sb->sb_ssize) / slos->slos_sb->sb_bsize) + 1;
	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		MPASS(error == 0);
		error = initialize_btree(slos, offset, &ptr);
		DEBUG1("Initializing checksum tree %lu", ptr.offset);
		MPASS(error == 0);
	} else {
		ptr = slos->slos_sb->sb_cksumtree;
	}

	// In case of remounting of a snapshot
	if (slos->slos_cktree != NULL) {
		slos_vpfree(slos, slos->slos_cktree);
	}

	DEBUG1("Loading Checksum tree %lu", ptr.offset);
	slos->slos_cktree = slos_vpimport(slos, ptr.offset);
	fbtree_init(slos->slos_cktree->sn_fdev, slos->slos_cktree->sn_tree.bt_root, sizeof(uint64_t),
		sizeof(uint32_t), &uint64_t_comp, "Checksum tree", 0, &slos->slos_cktree->sn_tree);

	fbtree_reg_rootchange(&slos->slos_cktree->sn_tree, &slsfs_root_rc, slos->slos_cktree);
	MPASS(slos->slos_cktree != NULL);
	if (slos->slos_sb->sb_epoch == EPOCH_INVAL) {
		MPASS(fbtree_size(&slos->slos_cktree->sn_tree) == 0);
	}

	return (0);
}

static int
slsfs_startupfs(struct mount *mp)
{
	int error;
	struct slsfsmount *smp = mp->mnt_data;

	if (smp->sp_index == (-1)) {
		DEBUG("SLOS Read in Super");
		error = slos_sbread(&slos);
		if (error != 0) {
			DEBUG1("ERROR: slos_sbread failed with %d", error);
			if (slos.slos_cp != NULL) {
				g_topology_lock();
				g_vfs_close(slos.slos_cp);
				dev_ref(slos.slos_vp->v_rdev);
				g_topology_unlock();
			}
			return (-1);
		}
	} else {
		if (slos.slos_sb == NULL)
			slos.slos_sb = malloc(sizeof(struct slos_sb), M_SLOS_SB, M_WAITOK);
		slos_sbat(&slos, smp->sp_index, slos.slos_sb);
	}

	if (slos.slos_tq == NULL) {
		slos.slos_tq = taskqueue_create("SLOS Taskqueue", M_WAITOK, taskqueue_thread_enqueue, &slos.slos_tq);
		if (slos.slos_tq == NULL) {
			panic("Problem creating taskqueue");
		}
		DEBUG1("Creating taskqueue %p", slos.slos_tq);
	}
	/* 
	 * Initialize in memory the allocator and the vnode used for inode 
	 * bookkeeping.
	 */

	slsfs_checksumtree_init(&slos);
	slos_allocator_init(&slos);
	slsfs_inodes_init(mp, &slos);

	/*
	 * Start the threads, probably should have a sysctl to define number of 
	 * threads here.
	 */
	error = taskqueue_start_threads(&slos.slos_tq, 10, PVM, "SLOS Taskqueue Threads");
	if (error) {
		panic("%d issue starting taskqueue", error);
	}
	DEBUG("SLOS Loaded.");

	return error;
}

/*
 * Create an in-memory representation of the SLOS.
 */
static int
slsfs_create_slos(struct mount *mp, struct vnode *devvp)
{
	int error;

	lockinit(&slos.slos_lock, PVFS, "sloslock", VLKTIMEOUT, LK_NOSHARE);
	cv_init(&slos.slsfs_sync_cv, "SLSFS Syncer CV");
	mtx_init(&slos.slsfs_sync_lk, "syncer lock",  NULL, MTX_DEF);
	slos.slsfs_dirtybufcnt = 0;
	slos.slsfs_syncing = 0;
	slos.slsfs_mount = mp;

	slos.slos_vp = devvp;

	/* Hook up the SLOS into the GEOM provider for the backing device. */
	g_topology_lock();
	error = g_vfs_open(devvp, &slos.slos_cp, "slsfs", 1);
	if (error) {
		printf("Error in opening GEOM vfs");
		g_topology_unlock();
		return error;
	}
	slos.slos_pp = g_dev_getprovider(devvp->v_rdev);
	g_topology_unlock();

	error = slsfs_startupfs(mp);
	MPASS(error == 0);

	return (error);
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
		printf("Error creating SLOS - %d", error);
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

	DEBUG("Mounting Device Done");
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
	struct vnode *vp;

	/* Create the new inode in the filesystem. */
	error = slos_new_node(VPSLOS(dvp), mode, &pid);
	if (error) {
		return (error);
	}

	/* Get a vnode for the newly created inode. */
	error = slsfs_vget(dvp->v_mount, pid, LK_EXCLUSIVE, &vp);
	if (error) {
		return (error);
	}

	/* Inherit group id from parent directory */
	SLSVP(vp)->sn_ino.ino_gid = SLSVP(dvp)->sn_ino.ino_gid;
	if (creds != NULL) {
		SLSVP(vp)->sn_ino.ino_uid = creds->cr_uid;
	}
	DEBUG2("Creating file with gid(%lu) uid(%lu)", SLSVP(vp)->sn_ino.ino_gid, SLSVP(vp)->sn_ino.ino_uid);

	*vpp = vp;

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

	if (mp->mnt_data == NULL) {
		ronly = (mp->mnt_flag & MNT_RDONLY) != 0;

		/* Configure the filesystem to have the IO parameters of the device. */
		if (devvp->v_rdev->si_iosize_max != 0)
			mp->mnt_iosize_max = devvp->v_rdev->si_iosize_max;

		if (mp->mnt_iosize_max > MAXPHYS)
			mp->mnt_iosize_max = MAXPHYS;

		vfs_getopts(mp->mnt_optnew, "from", &error);
		if (error)
			goto error;

		/* Create the in-memory data for the filesystem instance. */
		smp = (struct slsfsmount *)malloc(sizeof(struct slsfsmount), M_SLSFSMNT, M_WAITOK | M_ZERO);

		smp->sp_index = -1;
		smp->sp_vfs_mount = mp;
		smp->sp_ronly = ronly;
		smp->sls_valloc = &slsfs_valloc;
		smp->sp_slos = &slos;
		mp->mnt_data = smp;

		DEBUG1("slsfs_mountfs(%p)", devvp);
		/* Create the in-memory data for the backing device. */
		DEBUG("Not a snap remount - mount device");
		error = slsfs_mount_device(devvp, mp, &slsfsdev);
		if (error) {
			return error;
		}

		smp->sp_sdev = slsfsdev;
	} else {
		smp = (struct slsfsmount *)mp->mnt_data;
		error = slsfs_startupfs(mp);
	}

	KASSERT(smp->sp_slos != NULL, ("Null slos"));
	if (error)
		return error;

	mp->mnt_data = smp;

	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_USES_BCACHE;
	MNT_IUNLOCK(mp);

#ifdef SLOS_TEST

	printf("Testing fbtreecomponent...");
	error = slsfs_fbtree_test();
	if (error != 0)
		printf("ERROR: Test failed with %d", error);

	/* XXX Returning an error locks up the mounting process. */

#endif /* SLOS_TEST */

	VOP_UNLOCK(slos.slos_vp, 0);

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
 *
 * Checkpointing should follow this procedure
 * 1. Sync all data - this data is marked with the current snapshot so writes to 
 * the same block during the same epoch don't incur another allocation.
 *
 * 2. Mark all BTree buffers CoW - we can do this as we are managing these 
 * buffers so we can mark the buffers as CoW.  So the next time we modify the 
 * Btree it will make an allocation then and move the buffer over to the need 
 * logical block.  
 *
 * Btree's also get the advantage that since logical is physical we can cheat 
 * and actually not copy the data to a new buffer. When the btrees get flushed, 
 * once they get marked for CoW, we simply allocate, then move the buffer to the 
 * correct logical block number within its buffer object (simply removing it 
 * then inserting it with the new logical block number)
 *
 * 3. Sync all the Inodes (Syncing the SLOS_INODES_ROOT inode)
 *
 * 4. Now we have to sync the allocator btrees this, so we actually have to 
 * preallocate what we will need as this may modify and dirty more data of the 
 * underlying allocator btrees, we then sync the btrees with the pre-allocated 
 * spaces.  We don't use the trick above as this causes a circular dependency of 
 * marking a allocator node as CoW, then needed to modify the same CoW btree 
 * node for a new allocation.  
 *
 * There may be a way to avoid this pre allocation
 * and deal with it later but I think this would require a secondary path within
 * the btrees to know that it is the allocator and it would clear the CoW flag 
 * and continue the write but then allocate a new location for itself after the 
 * write is done.
 *
 * 5. Retrieve next superblock and update it with the proper information of 
 * where the inodes root is, as well as the new allocation tree roots (these 
 * should aways be dirty in a dirty checkpoint), update epoch and done.
 */

uint64_t checkpoints = 0;

static void
slsfs_checkpoint(struct mount *mp, int closing)
{
	struct vnode *vp, *mvp = NULL;
	struct buf *bp;
	struct slos_node *svp;
	struct slos_inode *ino;
	struct timespec te;
	diskptr_t ptr;
	int error;

again:
	DEBUG("Checkpointing vnodes");

	/* Go throught the list of vnodes attached to the filesystem. */
	MNT_VNODE_FOREACH_ACTIVE(vp, mp, mvp) {
		/* If we can't get a reference, the vnode is probably dead. */
		if (vp->v_type == VNON) {
			VI_UNLOCK(vp);
			continue;
		}

		if (vp == slos.slsfs_inodes) {
			VI_UNLOCK(vp);
			continue;
		}

		if ((error = vget(vp, LK_EXCLUSIVE | LK_INTERLOCK | LK_NOWAIT, curthread)) != 0) {
			if (error == ENOENT) {
				MNT_VNODE_FOREACH_ALL_ABORT(mp, mvp);
				goto again;
			}
			continue;
		}

		// Skip over the btrees for now as we will sync them after the 
		// data syncs
		if ((vp->v_vflag & VV_SYSTEM) || ((vp->v_type != VDIR) && (vp->v_type != VREG)) || (vp->v_data == NULL)) {
			vput(vp);
			continue;
		}


		if (SLSVP(vp)->sn_status & SLOS_DIRTY) {
			/* Step 1 and 2 Sync data and mark underlying Btree Copy 
			 * on write*/
			error = slos_sync_vp(vp, closing);
			if (error) {
				vput(vp);
				return;
			}

			/* Sync of data and btree complete - unmark them and 
			 * update the root and dirty the root*/
			error = slos_update(SLSVP(vp));
			if (error) {
				vput(vp);
				return;
			}
		}
		vput(vp);
	}

	// Check if both the underlying Btree needs a sync or the inode itself - 
	// should be a way to make it the same TODO
	// Just a hack for now to get this thing working XXX Why is it a hack?
	/* Sync the inode root itself. */
	if (slos.slos_sb->sb_data_synced) {
		error = slos_blkalloc(&slos, BLKSIZE(&slos), &ptr);
		MPASS(error == 0);

		DEBUG("Checkpointing the inodes btree");
		/* 3 Sync Root Inodes and btree */
		error = vn_lock(slos.slsfs_inodes, LK_EXCLUSIVE);
		if (error) {
			panic("vn_lock failed");
		}
		svp = SLSVP(slos.slsfs_inodes);
		ino = &svp->sn_ino;
		DEBUG2("Flushing inodes %p %p", slos.slsfs_inodes, svp->sn_fdev);
		error = slos_sync_vp(slos.slsfs_inodes, closing);
		if (error) {
			panic("slos_sync_vp failed to checkpoint");
			return;
		}

		/* 
		 * Allocate a new blk for the root inode write it and give it 
		 * to the superblock
		 */
		// Write out the root inode
		DEBUG("Creating the new superblock");
		ino->ino_blk = ptr.offset;

		slos.slos_sb->sb_root.offset = ino->ino_blk;

		bp = getblk(svp->sn_fdev, ptr.offset, BLKSIZE(&slos), 0, 0, 0);
		MPASS(bp);
		memcpy(bp->b_data, ino, sizeof(struct slos_inode));
		bawrite(bp);

		DEBUG("Checkpointing the checksum tree");
		// Write out the checksum tree;
		error = slos_blkalloc(&slos, BLKSIZE(&slos), &ptr);
		if (error) {
			panic("Problem with allocation");
		}

		DEBUG1("Flushing checksum %p", slos.slos_cktree->sn_tree.bt_backend);
		ino = &slos.slos_cktree->sn_ino;
		MPASS(slos.slos_cktree != SLSVP(slos.slsfs_inodes));
		ino->ino_blk = ptr.offset;
		if (checksum_enabled)
			fbtree_sync(&slos.slos_cktree->sn_tree);
		bp = getblk(svp->sn_fdev, ptr.offset, BLKSIZE(&slos), 0, 0, 0);
		MPASS(bp);
		memcpy(bp->b_data, ino, sizeof(struct slos_inode));
		bawrite(bp);

		slos.slos_sb->sb_cksumtree = ptr;

		DEBUG1("Checksum tree at %lu", ptr.offset);
		DEBUG1("Root Dir at %lu", SLSVP(slos.slsfs_inodes)->sn_ino.ino_blk);
		DEBUG1("Inodes File at %lu", slos.slos_sb->sb_root.offset);

		MPASS(ptr.offset != slos.slos_sb->sb_root.offset);

		slos.slos_sb->sb_index = (slos.slos_sb->sb_epoch) % 100;

		/* 4 Sync the allocator */
		DEBUG("Syncing the allocator");
		slos_allocator_sync(&slos, slos.slos_sb);
		DEBUG2("Epoch %lu done at superblock index %u", slos.slos_sb->sb_epoch, 
			slos.slos_sb->sb_index);
		SLSVP(slos.slsfs_inodes)->sn_status &= ~(SLOS_DIRTY);

		bp = getblk(slos.slos_vp, slos.slos_sb->sb_index, slos.slos_sb->sb_bsize, 0, 0, 0);
		MPASS(bp);
		memcpy(bp->b_data, slos.slos_sb, sizeof(struct slos_sb));

		DEBUG("Flushing the checksum tree again");
		if (checksum_enabled)
			fbtree_sync(&slos.slos_cktree->sn_tree);

		nanotime(&te);
		slos.slos_sb->sb_time = te.tv_sec;
		slos.slos_sb->sb_time_nsec = te.tv_nsec;

		bbarrierwrite(bp);

		VOP_UNLOCK(slos.slsfs_inodes, 0);

		checkpoints++;
		DEBUG3("Checkpoint: %lu, %lu, %lu", checkpoints, slos.slos_sb->sb_data_synced, slos.slos_sb->sb_meta_synced);

		slos.slos_sb->sb_data_synced = 0;
		slos.slos_sb->sb_meta_synced = 0;
		slos.slos_sb->sb_attempted_checkpoints = 0;
		slos.slos_sb->sb_epoch += 1;
	} else {
		slos.slos_sb->sb_attempted_checkpoints++;
	}
	DEBUG("Checkpoint done");

}

uint64_t checkpointtime = 100;
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
	struct timespec ts, te;
	uint64_t elapsed, period;

		/* Periodically sync until we unmount. */
	mtx_lock(&slos->slsfs_sync_lk);
	while (!slos->slsfs_sync_exit) {
		slos->slsfs_syncing = 1;
		mtx_unlock(&slos->slsfs_sync_lk);
		nanotime(&ts);
		slsfs_checkpoint(slos->slsfs_mount, 0);
		nanotime(&te);
		/* Notify anyone waiting to synchronize. */
		mtx_lock(&slos->slsfs_sync_lk);
		slos->slsfs_syncing = 0;
		cv_broadcast(&slos->slsfs_sync_cv);
		mtx_unlock(&slos->slsfs_sync_lk);

		elapsed = (1000000000ULL * (te.tv_sec - ts.tv_sec)) + (te.tv_nsec - ts.tv_nsec);

		period = checkpointtime * 1000000ULL;

		te.tv_sec = 0;
		if (elapsed < period) {
			te.tv_nsec = period - elapsed;
		} else {
			te.tv_nsec = 0;
		}

		/* Wait until it's time to flush again. */
		mtx_lock(&slos->slsfs_sync_lk);
		msleep_sbt(&slos->slsfs_syncing, &slos->slsfs_sync_lk, 
			PRIBIO, "Sync-wait", SBT_1NS * te.tv_nsec, 0, C_HARDCLOCK);
	}

	DEBUG("Syncer exiting");
	slos->slsfs_syncing = 1;
	mtx_unlock(&slos->slsfs_sync_lk);
	/* One last checkpoint before we exit. */
	slsfs_checkpoint(slos->slsfs_mount, 1);

	mtx_lock(&slos->slsfs_sync_lk);
	/* Notify anyone else waiting to flush one last time. */
	slos->slsfs_syncing = 0;
	DEBUG("Wake- up external");
	cv_broadcast(&slos->slsfs_sync_cv);

	DEBUG("Syncer exited");
	slos->slsfs_syncertd = NULL;
	mtx_unlock(&slos->slsfs_sync_lk);
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

	/* Initialize the root directory on first mount */
	if (SLSINO(SLSVP(vp)).ino_nlink < 2) {
		slsfs_init_dir(vp, vp, NULL);
	}

	vput(vp);

	return (0);
}

/*
 * Wake up the SLOS syncer.
 */
static int
slsfs_wakeup_syncer(int is_exiting)
{
	/* Don't sync again if already in progress. */
	/* XXX Maybe exit if it's already in progress? How do we
	 * serialize writes and syncs? (If a write after the last
	 * sync but before this one doesn't get  written by the former
	 * then we have to go through with the latter).
	 */

	mtx_lock(&slos.slsfs_sync_lk);
	if (slos.slsfs_syncertd == NULL) {
		mtx_unlock(&slos.slsfs_sync_lk);
		return (0);
	}

	if (slos.slsfs_syncing) {
		cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	}

	slos.slsfs_syncing = 1;
	if (is_exiting) {
		slos.slsfs_sync_exit = 1;
	}

	/* The actual wakeup. */
	wakeup(&slos.slsfs_syncing);

	if (slos.slsfs_syncertd == NULL) {
		mtx_unlock(&slos.slsfs_sync_lk);
		return (0);
	}

	/* Wait until the syncer notifies us it's done. */
	cv_wait(&slos.slsfs_sync_cv, &slos.slsfs_sync_lk);
	mtx_unlock(&slos.slsfs_sync_lk);

	return (0);
}

/*
 * Mount the filesystem.
 */
static int
slsfs_mount(struct mount *mp)
{
	DEBUG("Mounting slsfs");
	struct vnode *devvp = NULL;
	struct nameidata nd;
	struct vfsoptlist *opts;
	int error = 0;
	char *from;

	DEBUG("Mounting drive");

	if (mp->mnt_data != NULL)  {
		return (0);
	}

	if (mp->mnt_flag & MNT_UPDATE) {
		return (0);
	} else if (mp->mnt_data != NULL) {
		slsfs_wakeup_syncer(1);
		vflush(mp, 0, FORCECLOSE, curthread);
		VOP_LOCK(slos.slsfs_inodes, LK_EXCLUSIVE);
		VOP_RECLAIM(slos.slsfs_inodes, curthread);
		VOP_UNLOCK(slos.slsfs_inodes, 0);
		VOP_LOCK(slos.slos_vp, LK_EXCLUSIVE);
		error = slsfs_mountfs(slos.slos_vp, mp);
		MPASS(error == 0);

		error = slsfs_init_fs(mp);
		MPASS(error == 0);
	} else {
		opts = mp->mnt_optnew;
		vfs_filteropt(opts, slsfs_opts);

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
			DEBUG("Is not a disk");
			vput(devvp);
			return (error);
		}

		/* Get an ID for the new filesystem. */
		vfs_getnewfsid(mp);
		/* Actually mount the vnode as a filesyste and initialize its 
		 * state. */
		error = slsfs_mountfs(devvp, mp);
		if (error) {
			return (error);
		}

		error = slsfs_init_fs(mp);
		if (error) {
			/* XXX: This seems like a broken error path */
			return (error);
		}

		/* Get the path where we found the device. */
		vfs_mountedfrom(mp, from);
	}
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
	sbp->f_namemax = SLSFS_NAME_LEN;

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

	slos_allocator_uninit(&slos);

	/* Destroy related in-memory locks. */
	lockdestroy(&slos.slos_lock);

	free(slos.slos_sb, M_SLOS_SB);

	/* Destroy the device. */
	mtx_destroy(&sdev->g_mtx);

	/* Restore the node's private data. */
	sdev->devvp->v_data = sdev->vdata;

	free(sdev, M_SLSFSMNT);

	DEBUG("Device Unmounted");

	return error;
}

/*
 * Destroy the mounted filesystem data.
 */
static void
slsfs_freemntinfo(struct mount *mp)
{
	DEBUG("Destroying slsmount info");
	struct slsfsmount *smp = TOSMP(mp);

	if (mp == NULL)
		return;

	free(smp, M_SLSFSMNT);
	DEBUG("Destroyed slsmount info");
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
	struct slos_node *svp;
	int error;
	int flags = 0;

	smp = mp->mnt_data;
	sdev = smp->sp_sdev;
	slos = smp->sp_slos;

	/*
	 * XXX This shouldn't really be here, but the SLOS and the FS are not two discrete
	 * components yet. The right way to do this is get a reference to the SLOS from the
	 * filesystem, and allow the SLOS to be destroyed only after the filesystem has been
	 * unmounted.
	 *
	 * Disallow unmounting the FS while the SLS is still active. If we
	 * want to honor MNT_FORCE we can sleep, but we definitely can't remove
	 * the mount point from below the SLS.
	 */
	if (slos->slos_usecnt > 0)
		return (EBUSY);

	if (mntflags & MNT_FORCE) {
		flags |= FORCECLOSE;
	}

	DEBUG("UNMOUNTING");

	/* Free the slos taskqueue */
	taskqueue_free(slos->slos_tq);
	slos->slos_tq = NULL;

	/*
	 * Flush the data to the disk. We have already removed all
	 * vnodes, so this is going to be the last flush we need.
	 */
	slsfs_wakeup_syncer(1);
	/* 
	 * Seems like we don't call reclaim on a reference count drop so I 
	 * manually call slos_vpfree to release the memory.
	 */
	vrele(slos->slsfs_inodes);

	error = vflush(mp, 0, flags, curthread);
	if (error) {
		return (error);
	}

	slos->slsfs_inodes = NULL;
	// Free the checksum tree
	svp = slos->slos_cktree;
	slos->slos_cktree = NULL;
	slos_vpfree(slos, svp);

	DEBUG("Flushed all active vnodes");
	/* Remove the mounted device. */
	error = slsfs_unmount_device(sdev);
	if (error) {
		return (error);
	}

	cv_destroy(&slos->slsfs_sync_cv);
	mtx_destroy(&slos->slsfs_sync_lk);
	slsfs_freemntinfo(mp);

	DEBUG("Freeing mount info");
	mp->mnt_data = NULL;
	DEBUG("Changing mount flags");


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
		vp->v_type = VDIR;
		SLSVP(vp)->sn_ino.ino_gid = 0;
		SLSVP(vp)->sn_ino.ino_uid = 0;
	} else if (ino == SLOS_INODES_ROOT) {
		vp->v_type = VREG;
		vp->v_vflag |= VV_SYSTEM;
	} else {
		vp->v_type = IFTOVT(mp->sn_ino.ino_mode);
	}

	if (vp->v_type == VFIFO) {
		vp->v_op = &slfs_fifoops;
	}

	vnode_create_vobject(vp, 0, curthread);
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
	error = slos_get_node(&slos, ino, &svnode);
	if (error) {
		*vpp = NULL;
		return (error);
	}

	/* Get a new blank vnode. */
	error = getnewvnode("slsfs", mp, &slfs_vnodeops, &vp);
	if (error) {
		DEBUG("Problem getting new inode");
		*vpp = NULL;
		return (error);
	}

	/*
	 * If the vnode is not the root, which is managed directly
	 * by the SLOS, add it to the mountpoint.
	 */
	vn_lock(vp, LK_EXCLUSIVE);
	if (ino != SLOS_INODES_ROOT) {
		error = insmntque(vp, mp);
		if (error) {
			DEBUG("Problem queing root into mount point");
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
			DEBUG("Problem with vfs hash insert");
			*vpp = NULL;
			return (error);
		}
	}
	DEBUG2("vget(%p) ino = %ld", vp, ino);
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

static struct vfsops slfs_vfsops = {
	.vfs_init =		slsfs_init,
	.vfs_uninit =		slsfs_uninit,
	.vfs_root =		slsfs_root,
	.vfs_statfs =		slsfs_statfs,
	.vfs_mount =		slsfs_mount,
	.vfs_unmount =		slsfs_unmount,
	.vfs_vget =		slsfs_vget,
	.vfs_sync =		slsfs_sync
};

VFS_SET(slfs_vfsops, slsfs, 0);
MODULE_VERSION(slsfs, 0);
