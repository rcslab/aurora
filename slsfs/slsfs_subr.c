#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <vm/uma.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/namei.h>
#include <sys/rwlock.h>
#include <sys/bio.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include <machine/atomic.h>

#include <slos.h>
#include <slos_io.h>
#include <slos_record.h>
#include <slosmm.h>

#include "../slos/slos_alloc.h"

#include "slsfs.h"
#include "slsfs_subr.h"
#include "slsfs_dir.h"
#include "slsfs_buf.h"
#include "slsfs_node.h"

uint64_t pids;

struct buf_ops bufops_slsfs = {
	.bop_name	=   "slsfs_bufops",
	.bop_strategy	=   bufstrategy, 
	.bop_write	=   slsfs_bufwrite,
	.bop_sync	=   slsfs_bufsync,
	.bop_bdflush	=   slsfs_bdflush,
};

int 
slsfs_init(struct vfsconf *vfsp)
{
	pids = SLOS_ROOT_INODE + 1;
	return (0);
}

int 
slsfs_uninit(struct vfsconf *vfsp)
{
	return (0);
}

int
slsfs_new_node(struct slos *slos, mode_t mode, uint64_t *ppid)
{
	//struct slos_node *vp;
	uint64_t pid;
	int error;

	if (*ppid == 0) {
		pid = atomic_fetchadd_64(&pids, 1);
	} else {
		pid = *ppid;
	}

	error = slos_icreate(slos, pid, mode);
	if (error) {
		*ppid = 0;
		return (error);
	}

	*ppid = pid;

	return (0);
}

int
slsfs_remove_node(struct vnode *dvp, struct vnode *vp, struct componentname *name)
{
	int error;
	struct slos_node *svp = SLSVP(vp);

	error = slsfs_unlink_dir(dvp, vp, name);
	if (error) {
		return error;
	}
	error = slos_iremove(svp->sn_slos, SLSINO(svp)->ino_pid);
	if (error) {
		return (error);
	}
	/* Free the slos_node, including the in memory version of the inode */
	return (0); 
}


int
slsfs_getnode(struct slos *slos, uint64_t pid, struct slos_node **spp)
{
	struct slos_node *vp;

	vp = slos_iopen(slos, pid);
	if (vp != NULL) {
		*spp = vp;
		return (0);
	}

	return (EINVAL);
}

int 
slsfs_bufwrite(struct buf *buf)
{
	return bufwrite(buf);
}

int 
slsfs_bufsync(struct bufobj *bufobj, int waitfor)
{
	return slsfs_sync_vp(bo2vnode(bufobj));
}

void
slsfs_bdflush(struct bufobj *bufobj, struct buf *buf)
{    
	bufbdflush(bufobj, buf);
}

int
slsfs_sync_vp(struct vnode *vp)
{
	struct buf *bp, *tbd;
	struct slos_diskptr ptr;
	struct slos_recentry entry;

	struct slos *slos = SLSVP(vp)->sn_slos;
	struct bufobj *bo = &vp->v_bufobj;
	size_t blksize = IOSIZE(SLSVP(vp));
	int error = 0;

	BO_LOCK(bo);
	TAILQ_FOREACH_SAFE(bp, &bo->bo_dirty.bv_hd, b_bobufs, tbd) {
		if (BUF_LOCK(bp, LK_EXCLUSIVE | LK_INTERLOCK | LK_SLEEPFAIL, BO_LOCKPTR(bo)) == ENOLCK) {
			continue;
		}

		ptr = slos_alloc(slos->slos_alloc, 1);
		entry.diskptr = ptr;
		entry.len = blksize;
		entry.offset = 0;

		error = slsfs_key_replace(SLSVP(vp), bp->b_lblkno, entry);
		if (error) {
			BUF_UNLOCK(bp);
			continue;
		}
		bp->b_blkno = ptr.offset;
		/* This bwrite will call bstrategy */
		slsfs_bundirty(bp);
		BO_LOCK(bo);
	}
	BO_UNLOCK(bo);

	return slos_iupdate(SLSVP(vp));
}