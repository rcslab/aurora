#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/param.h>

#include <sys/cdefs.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/tty.h>
#include <sys/unistd.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

/* XXX Pipe has to be after selinfo */
#include <sys/pipe.h>

/* 
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <sls_data.h>

#include "sls_internal.h"
#include "sls_file.h"
#include "sls_vnode.h"

#include "debug.h"

SDT_PROBE_DEFINE(sls, , , namestart);
SDT_PROBE_DEFINE(sls, , , nameend);
SDT_PROBE_DEFINE(sls, , , nameerr);

/*
 * Check if the vnode is a TTY.
 */
bool
slsckpt_vnode_istty(struct vnode *vp)
{
	if (vp->v_type != VCHR)
		return (false);

	return (vp->v_rdev->si_devsw->d_flags & D_TTY) != 0;
}

/*
 * Check if the vnode can be checkpointed by name. 
 */
bool
slsckpt_vnode_ckptbyname(struct vnode *vp)
{
	/* If it's a regular file, we're good. */
	if (vp->v_type == VREG)
		return (true);

	/* Same if it's a FIFO. */
	if (vp->v_type == VFIFO)
		return (true);

	/* Also checkpoint directories, they have a path after all. */
	if (vp->v_type == VDIR)
		return (true);

	/* 
	 * If it's a character device, make sure it's not a TTY; TTYs are a 
	 * special case handled in an other path.
	 */
	if (vp->v_type == VCHR)
		return (!slsckpt_vnode_istty(vp));

	/* We don't handle any other cases. */
	return (false);
}

int
slsckpt_vnode(struct vnode *vp, struct slsckpt_data *sckpt_data)
{
	struct thread *td = curthread;
	struct sls_record *rec;
	char *freepath = NULL;
	char *fullpath = "";
	struct slsvnode slsvnode;
	struct sbuf *sb;
	int error;

	/* Check if using the name/inode number makes sense. */
	if (!slsckpt_vnode_ckptbyname(vp))
		return (EINVAL);

	/* Check if we have already checkpointed this vnode. */
	if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t) vp, (uintptr_t *) &rec) == 0)
		return (0);

	sb = sbuf_new_auto();

	/* Use the inode number for SLOS nodes, paths for everything else. */
	if (vp->v_mount != slos.slsfs_mount) {
		/* Successfully found a path. */
		slsvnode.magic = SLSVNODE_ID;
		slsvnode.slsid = (uint64_t) vp;
		slsvnode.has_path = 1;
		error = vn_fullpath(td, vp, &fullpath, &freepath);
		if (error != 0) {
			DEBUG2("vn_fullpath() failed for vnode %p with error %d", vp, error);
			if (SLSATTR_ISIGNUNLINKED(sckpt_data->sckpt_attr))
				panic("Unlinked vnode %p not in the SLOS", vp);

			/* Otherwise clean up as if after an error. */
			slsvnode.ino = 0;
			error = 0;
			goto error;
		}

		/* Write out the struct file. */
		strncpy(slsvnode.path, fullpath, PATH_MAX);
		error = sbuf_bcat(sb, (void *) &slsvnode, sizeof(slsvnode));
		if (error != 0)
			goto error;

		DEBUG("Checkpointing vnode by path");
	} else {
		slsvnode.magic = SLSVNODE_ID;
		slsvnode.slsid = (uint64_t) vp;
		slsvnode.has_path = 0;
		slsvnode.ino = INUM(SLSVP(vp));
		DEBUG1("Checkpointing vnode with inode 0x%lx", slsvnode.ino);
	}

	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *) &slsvnode, sizeof(slsvnode));
	if (error != 0)
		goto error;

	sbuf_finish(sb);

	/* Whether we have a path or not, create the new record. */
	rec = sls_getrecord(sb, slsvnode.slsid, SLOSREC_VNODE);
	error = slskv_add(sckpt_data->sckpt_rectable, slsvnode.slsid, (uintptr_t) rec);
	if (error != 0) {
		free(rec, M_SLSREC);
		goto error;
	}

	free(freepath, M_TEMP);
	return (0);

error:
	sbuf_delete(sb);

	free(freepath, M_TEMP);
	return (error);
}

/*
 * Get the vnode corresponding to a VFS path.
 */
static int
slsrest_vnode_path(struct slsvnode *info, struct vnode **vpp)
{
	struct thread *td = curthread;
	struct nameidata nd;
	cap_rights_t rights;
	int error;

	NDINIT_ATRIGHTS(&nd, LOOKUP, FOLLOW | AUDITVNODE1, UIO_SYSSPACE,
	    info->path, AT_FDCWD, &rights, td);
	error = namei(&nd);
	*vpp = nd.ni_vp;
	NDFREE(&nd, NDF_ONLY_PNBUF);

	return (error);
}

/*
 * Restore a vnode from a SLOS inode. This is mostly code from kern_openat()
 */
static int
slsrest_vnode_ino(struct slsvnode *info, struct vnode **vpp)
{
	struct vnode *vp;
	int error;

	KASSERT(info->has_path == 0, ("restoring linked file by inode number"));

	DEBUG1("Restoring vnode backed by inode %lx", info->ino);

	/* Try to get the vnode from the SLOS. */
	error = VFS_VGET(slos.slsfs_mount, info->ino, LK_EXCLUSIVE, &vp);
	if (error != 0)
		return (error);

	*vpp = vp;
	VOP_UNLOCK(*vpp, 0);

	return (0);
}

int
slsrest_vnode(struct slsvnode *info, struct slsrest_data *restdata)
{
	struct vnode *vp;
	int error;

	/* Get the vnode either from the inode or the path. */
	if (info->has_path == 1) {
		DEBUG("Restoring named vnode");
		error = slsrest_vnode_path(info, &vp);
	} else {
		DEBUG1("Restoring vnode with inode number 0x%lx", info->ino);
		error = slsrest_vnode_ino(info, &vp);
	}
	if (error != 0)
		return (error);

	/* Add it to the table of open vnodes. */
	error = slskv_add(restdata->vnodetable, info->slsid, (uintptr_t) vp);
	if (error != 0) {
		vrele(vp);
		return (error);
	}

	return (0);
}
