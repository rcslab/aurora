#include <sys/param.h>
#include <sys/bio.h>
#include <sys/bitstring.h>
#include <sys/buf.h>
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/swap_pager.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>
#include <vm/vm_param.h>

#include <slos_io.h>

#include "debug.h"
#include "sls_internal.h"
#include "sls_pager.h"
#include "sls_prefault.h"

#define SLS_SWAPOFF_RETRIES (10)
#define SLS_VMOBJ_SWAPVP(obj) ((obj)->un_pager.swp.swp_tmpfs)

/* The initial swap pager operations vector. */
static struct pagerops swappagerops_old;

/*
 * Reference the module, signaling it is in use.
 */
static inline int
sls_swapref(void)
{
	SLS_LOCK();
	if (SLS_EXITING() != 0) {
		SLS_UNLOCK();
		return (EBUSY);
	}
	slsm.slsm_swapobjs += 1;
	SLS_UNLOCK();

	return (0);
}

/*
 * Remove a reference to the module owned by a swap object.
 */
static inline void
sls_swapderef(void)
{
	SLS_LOCK();
	KASSERT(slsm.slsm_swapobjs > 0, ("module has no references left"));
	slsm.slsm_swapobjs -= 1;
	cv_broadcast(&slsm.slsm_exitcv);
	SLS_UNLOCK();
}

/*
 * Mark the buffer as invalid and wake up any waiters on the object.
 */
static void
sls_pager_done(struct buf *bp)
{
	vm_object_t obj = NULL;
	vm_pindex_t start, end;
	uint64_t objid = 0;
	uint64_t npages;
	vm_page_t m;
	int i;

	npages = bp->b_npages;
	if (bp->b_npages > 0) {
		start = bp->b_pages[0]->pindex;
		end = bp->b_pages[bp->b_npages - 1]->pindex;

		obj = bp->b_pages[0]->object;
		objid = obj->objid;
		VM_OBJECT_WLOCK(obj);
		KASSERT(obj->type != OBJT_DEAD, ("object is dead"));

		for (i = 0; i < bp->b_npages; i++) {
			KASSERT(
			    (bp->b_flags & B_INVAL) == 0, ("paging failed"));
			KASSERT(
			    bp->b_pages[i]->object == obj, ("wrong object"));

			m = bp->b_pages[i];
			m->oflags &= ~VPO_SWAPINPROG;
			if (m->oflags & VPO_SWAPSLEEP) {
				m->oflags &= ~VPO_SWAPSLEEP;
				wakeup(&obj->paging_in_progress);
			}
			KASSERT(
			    (bp->b_ioflags & BIO_ERROR) == 0, ("swap failed"));
			KASSERT(
			    bp->b_iocmd == BIO_READ || bp->b_iocmd == BIO_WRITE,
			    ("invalid BIO operation %d", bp->b_iocmd));

			if (bp->b_iocmd == BIO_READ) {
				m->valid = VM_PAGE_BITS_ALL;

				/* If speculatively paged in, deactivate. */
				if (i < bp->b_pgbefore ||
				    i >= bp->b_npages - bp->b_pgafter)
					vm_page_readahead_finish(m);
			} else if (bp->b_iocmd == BIO_WRITE) {
				/*
				 * Apply the same heuristics as the swapper.
				 * Lots of room for optimization here, depending
				 * on what we do with the page lists!
				 */
				vm_page_undirty(m);
				vm_page_lock(m);
				vm_page_deactivate_noreuse(m);
				vm_page_unlock(m);
			}
		}
		vm_object_pip_wakeupn(obj, bp->b_npages);
		VM_OBJECT_WUNLOCK(obj);
	}

	bp->b_npages = 0;
	bp->b_bcount = bp->b_bufsize = 0;
	/* Disassociate from the VM object ID. */
	bp->b_aurobj = NULL;

	bdone(bp);

	if (objid != 0)
		slspre_mark(objid, start, end);
}

/*
 * Set up the callback and do object reference counting. This needs to be called
 * from the SLS process context, so that we are certain we are holding a
 * reference to the VM object while we are doing this.
 */
static void
sls_pager_done_setup(struct buf *bp)
{
	vm_object_t obj;
	int i;

	obj = bp->b_pages[0]->object;
	VM_OBJECT_ASSERT_LOCKED(obj);

	vm_object_pip_add(obj, bp->b_npages);

	for (i = 0; i < bp->b_npages; i++)
		bp->b_pages[i]->oflags |= VPO_SWAPINPROG;

	bp->b_iodone = sls_pager_done;
	/* Associate with a VM object ID. */
	bp->b_aurobj = (void *)obj->objid;
}

struct buf *
sls_pager_readbuf(
    vm_object_t obj, vm_pindex_t pindex, size_t npages, bool *retry)
{
	struct buf *bp;
	int i;

	KASSERT(npages < btoc(MAXPHYS), ("target IO too large"));
	VM_OBJECT_ASSERT_WLOCKED(obj);

	bp = getpbuf(&slos_pbufcnt);
	if (bp == NULL) {
		*retry = true;
		return (NULL);
	}

	vm_page_grab_pages(obj, pindex, VM_ALLOC_NORMAL, bp->b_pages, npages);
	for (i = 0; i < npages; i++) {
		bp->b_pages[i]->valid = 0;
		bp->b_pages[i]->oflags |= VPO_SWAPINPROG;
	}

	bp->b_data = unmapped_buf;
	bp->b_npages = npages;
	bp->b_resid = bp->b_npages * PAGE_SIZE;
	bp->b_bcount = bp->b_bufsize = bp->b_resid;
	bp->b_lblkno = pindex + SLOS_OBJOFF;
	bp->b_iocmd = BIO_READ;

	sls_pager_done_setup(bp);
	*retry = false;

	return (bp);
}

/*
 * Create a buffer with the pages to be sent out.
 */
struct buf *
sls_pager_writebuf(
    vm_object_t obj, vm_pindex_t pindex, size_t targetsize, bool *retry)
{
	size_t npages = 0;
	vm_pindex_t pindex_init;
	struct buf *bp;
	vm_page_t m;

	/* Read the global variable once here to prevent possible races. */
	VM_OBJECT_ASSERT_WLOCKED(obj);

	KASSERT(targetsize <= sls_contig_limit,
	    ("page %lx is larger than sls_contig_limit %lx", targetsize,
		sls_contig_limit));
	KASSERT(
	    (targetsize / PAGE_SIZE) < btoc(MAXPHYS), ("target IO too large"));
	KASSERT(obj->ref_count >= obj->shadow_count + 1,
	    ("object has %d references and %d shadows", obj->ref_count,
		obj->shadow_count));

	bp = trypbuf(&slos_pbufcnt);
	if (bp == NULL) {
		*retry = true;
		return (NULL);
	}

	KASSERT(bp != NULL, ("did not get new physical buffer"));
	KASSERT(bp->b_resid == 0, ("Buffer already has resid"));

	for (m = vm_page_find_least(obj, pindex); m != NULL;
	     m = vm_page_next(m)) {
		if (npages == 0)
			pindex_init = m->pindex;

		KASSERT(npages < btoc(MAXPHYS), ("overran b_pages[] array"));
		KASSERT(m->object == obj,
		    ("page %p in object %p "
		     "associated with object %p",
			m, obj, m->object));
		KASSERT(pindex_init + npages == m->pindex,
		    ("pages in the buffer are not consecutive "
		     "(pindex should be %ld, is %ld)",
			pindex_init + npages, m->pindex));
		KASSERT(pagesizes[m->psind] <= PAGE_SIZE,
		    ("dumping page %p with size %ld", m, pagesizes[m->psind]));

		/*
		 * We do not need physical contiguity for pages
		 * since we insert each page separately.
		 */
		m->oflags |= VPO_SWAPINPROG;
		bp->b_pages[npages++] = m;
		bp->b_resid += pagesizes[m->psind];
		KASSERT(pagesizes[m->psind] == PAGE_SIZE,
		    ("unexpected page size %ld", pagesizes[m->psind]));
		if (bp->b_resid == targetsize)
			break;
	}

	/* We didn't find any pages. */
	if (npages == 0) {
		relpbuf(bp, &slos_pbufcnt);
		*retry = false;

		return (NULL);
	}

	bp->b_data = unmapped_buf;
	bp->b_npages = npages;
	bp->b_bcount = bp->b_bufsize = bp->b_resid;
	bp->b_lblkno = pindex_init + SLOS_OBJOFF;
	bp->b_iocmd = BIO_WRITE;

	/* Update stats. */
	sls_pages_grabbed += bp->b_npages;

	sls_pager_done_setup(bp);

	BUF_ASSERT_LOCKED(bp);

	*retry = false;

	return (bp);
}

/*
 * Turn an Aurora object into a swap object. To be called both from the swapping
 * code and the Aurora shadowing code.
 */
int
sls_pager_obj_init(vm_object_t obj)
{
	struct vnode *vp;
	uint64_t oid;
	int error;

	/* If it's already prepared, nothing else to do. */
	if ((obj->type == OBJT_SWAP) && ((obj->flags & OBJ_AURORA) != 0))
		return (0);

	if (sls_swapref())
		return (EBUSY);

	/* Nowhere in the kernel are pager objects given a handle. */
	KASSERT(obj->type == OBJT_DEFAULT,
	    ("initializing object %lx of type %d in the Aurora pager",
		obj->objid, obj->type));
	KASSERT(obj->handle == NULL,
	    ("anonymous object passed to Aurora pager has a handle"));

	/*
	 * Even if the object was a swap object that was initialized before we
	 * inserted the SLS, the pctrie is empty. There is no state to clean up.
	 * The only edge case here is if the object is part of tmpfs, in which
	 * case we can do nothing but crash; we are reusing the pointer for
	 * Aurora right now.
	 */
	KASSERT((obj->flags & OBJ_TMPFS) == 0, ("paging tmpfs object"));

	/*
	 * We reuse the pointer to the tmpfs already in the object for our own
	 * purposes. We can create a new field if this gets confusing.
	 */

	oid = obj->objid;
	error = slos_svpalloc(&slos, MAKEIMODE(VREG, S_IRWXU), &oid);
	KASSERT(error == 0, ("error %d when allocating SLOS inode", error));

	error = VFS_VGET(slos.slsfs_mount, oid, 0, &vp);
	KASSERT(error == 0,
	    ("error %d when getting newly created SLOS inode", error));

	obj->type = OBJT_SWAP;
	obj->flags |= (OBJ_AURORA | OBJ_NOSPLIT);
	SLS_VMOBJ_SWAPVP(obj) = vp;

	DEBUG2("referenced %lx with vnode %p", obj->objid, vp);
	VOP_UNLOCK(vp, 0);

	/*
	 * We have the following problem: Top level objects are not in Aurora.
	 * If they start swapping, where do we put the data? The answer is that
	 * the top level object DOES have a backer in the SLOS, that of its
	 * parent, even if it isn't in Aurora. As long as it has a parent in
	 * Aurora, it can be swapped.
	 *
	 * Now, does it ALWAYS have a parent in Aurora? Possibly not, and in
	 * this case we prioritize swapping out pages in objects that are. If
	 * we're at a point where we don't have any easily dumpable pages, we're
	 * under such extreme memory pressure that Aurora's dumping cannot keep
	 * up and the system would go down if it was using regular swapping
	 * anyway.
	 */

	return (0);
}

static boolean_t
sls_pager_haspage(vm_object_t obj, vm_pindex_t pindex, int *before, int *after)
{
	struct vnode *vp = SLS_VMOBJ_SWAPVP(obj);

	/* Get the extent the page is in, if it exists. */
	return slos_hasblock(vp, pindex + SLOS_OBJOFF, before, after);
}

static void
sls_pager_putpages(
    vm_object_t obj, vm_page_t *ma, int count, int flags, int *rtvals)
{
	size_t target_pagecnt;
	struct buf *bp;
	int error, i;
	bool retry;
	bool sync;

	VM_OBJECT_ASSERT_WLOCKED(obj);

	if (sls_pager_obj_init(obj) != 0) {
		for (i = 0; i < count; i++)
			rtvals[i] = VM_PAGER_FAIL;
		return;
	}

	/* The pager process swaps synchronously, the rest can be async. */
	if (curproc != pageproc)
		sync = TRUE;
	else
		sync = (flags & VM_PAGER_PUT_SYNC) != 0;

	/* We can do contiguous IOs up to a certain number of pages. */
	target_pagecnt = min(count, sls_contig_limit / PAGE_SIZE);
	bp = sls_pager_writebuf(
	    obj, ma[0]->pindex, target_pagecnt * PAGE_SIZE, &retry);
	if (retry) {
		/* We're out of memory, try again. */
		for (i = target_pagecnt; i < count; i++)
			rtvals[i] = VM_PAGER_AGAIN;

		return;
	}

	for (i = 0; i < target_pagecnt; i++) {
		KASSERT(ma[i]->dirty == VM_PAGE_BITS_ALL,
		    ("swapping out clean page"));
		KASSERT(ma[i] == bp->b_pages[i], ("bp page array is wrong"));
		rtvals[i] = VM_PAGER_PEND;
	}

	for (i = target_pagecnt; i < count; i++)
		rtvals[i] = VM_PAGER_AGAIN;

	/*
	 * XXX We need to make certain this will succeed; VM_PAGER_PEND actually
	 * denotes success, so if we do this asynchronously we have to make sure
	 * it hits the disk.
	 */

	VM_OBJECT_WUNLOCK(obj);
	error = slos_iotask_create(SLS_VMOBJ_SWAPVP(obj), bp, sync);
	VM_OBJECT_WLOCK(obj);
	if (error != 0)
		panic("swapping failed with %d", error);
}

static void
sls_pager_clip_read(int count, int *rbehind, int *rahead)
{
	size_t space_needed = 0;
	size_t space_left = btoc(MAXPHYS) - count;

	if (rahead)
		space_needed += *rahead;

	/*
	 * Prioritize readahead. If readahead already depletes all available
	 * space, clip it and don't do any readbehind.
	 *
	 */
	if (space_left < space_needed) {
		*rahead -= (space_needed - space_left);
		if (*rbehind)
			*rbehind = 0;
		return;
	}

	/* Clip readbehind by the amount of pages we are missing. */
	if (rbehind) {
		space_needed += *rbehind;
	}

	if (space_left < space_needed)
		*rbehind -= (space_needed - space_left);
}

static int
sls_pager_getpages(
    vm_object_t obj, vm_page_t *ma, int count, int *rbehind, int *rahead)
{
	int maxahead, maxbehind, npages;
	vm_page_t mpred, msucc, m;
	vm_pindex_t pindex;
	struct buf *bp;
	int error, i;
	bool present;
	bool retry;

	VM_OBJECT_WUNLOCK(obj);
	present = sls_pager_haspage(obj, ma[0]->pindex, &maxbehind, &maxahead);
	VM_OBJECT_WLOCK(obj);

	/* Make sure we have all pages, not just the one we looked for. */
	if (!present)
		return (VM_PAGER_FAIL);

	KASSERT(count < btoc(MAXPHYS), ("pagein does not fit in a buffer"));
	if (count - 1 > maxahead)
		return (VM_PAGER_FAIL);

	/*
	 * XXX We can be more aggressive than regular swappers, and bring in
	 * more pages than requested.
	 */
	if (rahead != NULL) {
		/* The size of the extent not covered by the page array. */
		*rahead = imin(*rahead, maxahead - (count - 1));

		/* We are bound by the first resident page to the right. */
		msucc = TAILQ_NEXT(ma[count - 1], listq);
		if (msucc != NULL) {
			*rahead = imin(
			    *rahead, msucc->pindex - ma[count - 1]->pindex - 1);
		}
	}
	if (rbehind != NULL) {
		/* The size of the extent not covered by the page array. */
		*rbehind = imin(*rbehind, maxbehind);

		/* Bound by the first resident page to the left. */
		mpred = TAILQ_PREV(ma[0], pglist, listq);
		if (mpred != NULL)
			*rbehind = imin(
			    *rbehind, ma[0]->pindex - mpred->pindex - 1);
	}

	/* Bound readahead and readbehind so that the IO fits in one buffer. */
	sls_pager_clip_read(count, rbehind, rahead);

	/* Get the final*/
	pindex = ma[0]->pindex;
	npages = count;
	if (rbehind) {
		for (i = 1; i <= *rbehind; i++) {
			m = vm_page_alloc(obj, ma[0]->pindex - i,
			    VM_ALLOC_NORMAL | VM_ALLOC_NOBUSY);
			if (m == NULL)
				break;
		}

		*rbehind = i - 1;
		pindex -= *rbehind;
		npages += *rbehind;
	}
	if (rahead) {
		for (i = 1; i <= *rahead; i++) {
			m = vm_page_alloc(obj, ma[count - 1]->pindex + i,
			    VM_ALLOC_NORMAL | VM_ALLOC_NOBUSY);
			if (m == NULL)
				break;
		}

		*rahead = i - 1;
		npages += *rahead;
	}

	/*
	 * Unbusy the pages we were passed. We will busy them again in
	 * sls_pager_readbuf; we have the object lock, so nobody else can access
	 * the page right now anyway.
	 */
	VM_OBJECT_ASSERT_WLOCKED(obj);
	for (i = 0; i < count; i++)
		vm_page_xunbusy(ma[i]);

	do {
		bp = sls_pager_readbuf(obj, pindex, npages, &retry);
	} while (retry);

	if (bp == NULL)
		return (VM_PAGER_FAIL);

	for (i = 0; i < npages; i++)
		KASSERT(vm_page_xbusied(bp->b_pages[i]), ("page is not busy"));

	/* Readbehind/readahead information. Used to mark the pages as cold. */
	bp->b_pgbefore = (rbehind != NULL) ? *rbehind : 0;
	bp->b_pgafter = (rahead != NULL) ? *rahead : 0;

	VM_OBJECT_WUNLOCK(obj);
	error = slos_iotask_create(SLS_VMOBJ_SWAPVP(obj), bp, true);
	VM_OBJECT_WLOCK(obj);

	/* Wait until the pages are brought in. */
	while ((ma[0]->oflags & VPO_SWAPINPROG) != 0) {
		ma[0]->oflags |= VPO_SWAPSLEEP;
		if (VM_OBJECT_SLEEP(obj, &obj->paging_in_progress, PSWP,
			"swread", hz * 20)) {
			printf("sls_pager: waiting for buffer %p\n", bp);
		}
	}

	if (error != 0)
		return (VM_PAGER_ERROR);

	/* Confirm that all pages are valid. */
	for (i = 0; i < count; i++) {
		if (ma[i]->valid != VM_PAGE_BITS_ALL)
			return (VM_PAGER_ERROR);
	}

	return (VM_PAGER_OK);
}

static vm_object_t
sls_pager_alloc(void *handle, vm_offset_t size, vm_prot_t prot,
    vm_ooffset_t offset, struct ucred *cred)
{
	vm_object_t obj;
	uint64_t oldid;

	KASSERT(offset == 0, ("offset 0x%lx for swap object", offset));

	if (cred != NULL) {
		if (!swap_reserve_by_cred(size, cred))
			return (NULL);
		crhold(cred);
	}

	obj = vm_object_allocate(OBJT_DEFAULT, OFF_TO_IDX(PAGE_MASK + size));

	/*
	 * The handle is the new object ID we want to create. In no place in the
	 * kernel is the swap pager called with a non-NULL handle argument, so
	 * we can reuse the field freely.
	 */
	oldid = obj->objid;
	obj->objid = (uint64_t)handle;

	if (sls_pager_obj_init(obj) != 0)
		goto error;

	if (cred != NULL) {
		obj->cred = cred;
		obj->charge = size;
	}

	return (obj);

error:
	/*
	 * This error path is triggered when the SLS starts exiting. However,
	 * the error handling throughout the kernel assumes that the failure is
	 * due to running out of memory. This shouldn't matter too much since if
	 * a process is using the SLS pager is in Aurora, so it's dying real
	 * soon.
	 */
	obj->objid = oldid;
	VM_OBJECT_WUNLOCK(obj);
	if (cred != NULL) {
		swap_release_by_cred(size, cred);
		crfree(cred);
	}
	vm_object_deallocate(obj);

	return (NULL);
}

/*
 * Destroy an Aurora object.
 */
static void
sls_pager_dealloc(vm_object_t obj)
{
	struct vnode *vp;

	VM_OBJECT_ASSERT_LOCKED(obj);
	KASSERT((obj->flags & OBJ_DEAD) != 0, ("dealloc of reachable object"));

	/* This can be a swap object created before Aurora. */
	if ((obj->flags & OBJ_AURORA) == 0) {
		DEBUG1("Aurora pager deallocating non-Aurora object%lx",
		    obj->objid);
		(*swappagerops_old.pgo_dealloc)(obj);
		return;
	}

	vm_object_pip_wait(obj, "slsdea");

	if ((obj->flags & OBJ_AURORA) == 0) {
		DEBUG1("Aurora swap object %lx lost its flag", obj->objid);
		return;
	}

	obj->type = OBJT_DEAD;
	obj->flags &= ~OBJ_AURORA;

	/* Release the vnode backing the object, if it exists. */
	vp = SLS_VMOBJ_SWAPVP(obj);
	DEBUG2("dereferenced %lx with handle %p", obj->objid, vp);
	SLS_VMOBJ_SWAPVP(obj) = NULL;

	VM_OBJECT_WUNLOCK(obj);
	vrele(vp);
	sls_swapderef();
	VM_OBJECT_WLOCK(obj);
}

static struct pagerops slspagerops = {
	.pgo_alloc = sls_pager_alloc,
	.pgo_dealloc = sls_pager_dealloc,
	.pgo_getpages = sls_pager_getpages,
	.pgo_putpages = sls_pager_putpages,
	.pgo_haspage = sls_pager_haspage,
};

/*
 * Register the Aurora backend with the kernel.
 */
void
sls_pager_register(void)
{
	/*
	 * This code is called from inside the module initialization function,
	 * so the module is guaranteed to be present - no need to take a
	 * reference to the module.
	 */
	/*
	 * We cannot call swapon after replacing the swap methods by
	 * those of Aurora.
	 */
	sx_xlock(&swdev_syscall_lock);
	swap_swapon_enabled = false;
	sx_xunlock(&swdev_syscall_lock);
	swapoff_all();

	/*
	 * The default pager ops call the swap pager ops directly, so we change
	 * the the swap pager ops instead of just the pointer in pagertab.
	 * Existing swap objects not in Aurora will be properly modified to use
	 * it.
	 */
	swappagerops_old = swappagerops;
	swappagerops = slspagerops;
}

/*
 * Restore the original swap methods during module destruction, allow swapping
 * on regular swap devices.
 */
void
sls_pager_unregister(void)
{
	sx_xlock(&swdev_syscall_lock);
	swap_swapon_enabled = true;
	/* Swap back the old function vector if we modified it. */
	if (slspagerops.pgo_alloc == sls_pager_alloc)
		swappagerops = swappagerops_old;
	sx_xunlock(&swdev_syscall_lock);
}

static void
sls_pager_swapoff_one(void)
{
	vm_object_t obj;

	/*
	 * We currently kill all Aurora processes before swapping off.
	 * This means we are expecting no active objects here, and that
	 * this function only checks for live objects instead of killing them.
	 */
	mtx_lock(&vm_object_list_mtx);
	TAILQ_FOREACH (obj, &vm_object_list, object_list) {
		mtx_unlock(&vm_object_list_mtx);
		VM_OBJECT_WLOCK(obj);

		if (obj->flags & OBJ_AURORA) {
			DEBUG5(
			    "Aurora object %lx (vp %p) type %d (refcount %x, dead: %s)",
			    obj->objid, SLS_VMOBJ_SWAPVP(obj), obj->type,
			    obj->ref_count,
			    (obj->flags & OBJ_DEAD) ? "yes" : "no");
		}

		VM_OBJECT_WUNLOCK(obj);
		mtx_lock(&vm_object_list_mtx);
	}
	mtx_unlock(&vm_object_list_mtx);
}

void
sls_pager_swapoff(void)
{
	int retries = 0;

	SLS_ASSERT_LOCKED();

	/* We assume the only references left belong to the */
	while (slsm.slsm_swapobjs > 0) {
		SLS_UNLOCK();
		sls_pager_swapoff_one();

		retries += 1;
		if (retries == SLS_SWAPOFF_RETRIES) {
			DEBUG1(
			    "failed to destroy %d objects", slsm.slsm_swapobjs);
			break;
		}

		pause("slsswp", hz / 20);
		SLS_LOCK();
	}
}
