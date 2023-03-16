#include <sys/param.h>
#include <sys/selinfo.h>
#include <sys/conf.h>
#include <sys/domain.h>
#include <sys/endian.h>
#include <sys/event.h>
#include <sys/eventvar.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/pipe.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/queue.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/tty.h>
#include <sys/un.h>
#include <sys/unistd.h>
#include <sys/unpcb.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/uma.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>

#include <machine/param.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <sls_data.h>

#include "debug.h"
#include "sls_file.h"
#include "sls_internal.h"
#include "sls_table.h"
#include "sls_vnode.h"

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
static bool
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

	/* Named UNIX sockets are also allowed. */
	if (vp->v_type == VSOCK)
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

/*
 * Store a vnode in memory, without serializing. For disk checkpoints, we
 * serialize after the partition has resumed.
 */
int
slsckpt_vnode(struct vnode *vp, struct slsckpt_data *sckpt_data)
{
	int error;

	if (slsset_find(sckpt_data->sckpt_vntable, (uint64_t)vp) == 0)
		return (0);

	error = slsset_add(sckpt_data->sckpt_vntable, (uint64_t)vp);
	if (error != 0)
		return (EINVAL);

	vref(vp);

	return (0);
}

/*
 * Serialize a single vnode into a record, held in an sbuf.
 */
static int
slsckpt_vnode_serialize_single(
    struct vnode *vp, bool allow_unlinked, struct sbuf **sbp)
{
	struct thread *td = curthread;
	char *freepath = NULL;
	char *fullpath = "";
	struct slsvnode slsvnode;
	struct sbuf *sb;
	int error;

	/* Check if using the name/inode number makes sense. */
	if (!slsckpt_vnode_ckptbyname(vp))
		return (EINVAL);

	sb = sbuf_new_auto();

	/* Use the inode number for SLOS nodes, paths for everything else. */
	if (vp->v_mount != slos.slsfs_mount) {
		/* Successfully found a path. */
		slsvnode.magic = SLSVNODE_ID;
		slsvnode.slsid = (uint64_t)vp;
		slsvnode.has_path = 1;
		error = vn_fullpath(td, vp, &fullpath, &freepath);
		if (error != 0) {
			DEBUG2(
			    "vn_fullpath() failed for vnode %p with error %d",
			    vp, error);
			if (!allow_unlinked)
				panic("Unlinked vnode %p not in the SLOS", vp);

			/* Otherwise clean up as if after an error. */
			slsvnode.ino = 0;
			error = 0;
			goto out;
		}

		/* Write out the struct file. */
		strncpy(slsvnode.path, fullpath, PATH_MAX);
		error = sbuf_bcat(sb, (void *)&slsvnode, sizeof(slsvnode));
		if (error != 0)
			goto error;

	} else {
		slsvnode.magic = SLSVNODE_ID;
		slsvnode.slsid = (uint64_t)vp;
		slsvnode.has_path = 0;
		slsvnode.ino = INUM(SLSVP(vp));
		DEBUG1("Checkpointing vnode with inode 0x%lx", slsvnode.ino);
	}


out:
	/* Write out the struct file. */
	error = sbuf_bcat(sb, (void *)&slsvnode, sizeof(slsvnode));
	if (error != 0)
		goto error;

	sbuf_finish(sb);

	*sbp = sb;

	free(freepath, M_TEMP);
	return (0);

error:
	sbuf_delete(sb);

	free(freepath, M_TEMP);
	return (error);
}

int
slsckpt_vnode_serialize(struct slsckpt_data *sckpt_data)
{
	struct slskv_iter iter;
	struct vnode *vp;
	uintptr_t exists;
	struct sbuf *sb;
	int error;

	KVSET_FOREACH(sckpt_data->sckpt_vntable, iter, vp)
	{
		/* Check if we have already serialized this vnode. */
		if (slskv_find(sckpt_data->sckpt_rectable, (uint64_t)vp,
			&exists) == 0) {
			KV_ABORT(iter);
			return (0);
		}

		error = slsckpt_vnode_serialize_single(
		    vp, SLSATTR_ISIGNUNLINKED(sckpt_data->sckpt_attr), &sb);
		if (error != 0) {
			KV_ABORT(iter);
			return (error);
		}

		error = slsckpt_addrecord(
		    sckpt_data, (uint64_t)vp, sb, SLOSREC_VNODE);
		if (error != 0) {
			KV_ABORT(iter);
			sbuf_delete(sb);
			return (error);
		}
	}

	/* If we are caching the result, keep the vnode table. */
	if (SLSATTR_ISCACHEREST(sckpt_data->sckpt_attr))
		return (0);

	KVSET_FOREACH_POP(sckpt_data->sckpt_vntable, vp)
	vrele(vp);

	return (0);
}

/*
 * Get the vnode corresponding to a VFS path.
 */
static int
slsvn_restore_path(struct slsvnode *info, struct vnode **vpp)
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
slsvn_restore_ino(struct slsvnode *info, struct vnode **vpp)
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
slsvn_restore_vnode(struct slsvnode *info, struct slsckpt_data *sckpt)
{
	struct vnode *vp;
	int error;

	/* Get the vnode either from the inode or the path. */
	if (info->has_path == 1) {
		DEBUG("Restoring named vnode");
		error = slsvn_restore_path(info, &vp);
		if (error != 0)
			return (error);
	} else {
		DEBUG1("Restoring vnode with inode number 0x%lx", info->ino);
		error = slsvn_restore_ino(info, &vp);
		if (error != 0)
			return (error);
	}

	/* Add it to the table of open vnodes. */
	error = slskv_add(sckpt->sckpt_vntable, info->slsid, (uintptr_t)vp);
	if (error != 0) {
		vrele(vp);
		return (error);
	}

	return (0);
}

static int
slsvn_slsid(struct file *fp, uint64_t *slsidp)
{
	struct vnode *vp = fp->f_vnode;

	/*
	 * We use the device pointer as our ID for ttys; it's
	 * accessible by the master side, while the vnode isn't.
	 *
	 * More than one file may correspond to a vnode, so in
	 * case of a regular file we use the file pointer itself.
	 * The vnode record uses the vnode.
	 */
	if (slsckpt_vnode_istty(vp))
		*slsidp = (uint64_t)fp->f_vnode->v_rdev;
	else
		*slsidp = (uint64_t)fp;

	return (0);
}

static int
slsvn_checkpoint(
    struct file *fp, struct sbuf *sb, struct slsckpt_data *sckpt_data)
{
	struct vnode *vp = (struct vnode *)fp->f_vnode;
	int error;

	/*
	 * In our case, vnodes are either regular, or they are ttys
	 * (the slave side, the master side is DTYPE_PTS). In the former
	 * case, we just get the name; in the latter, the name is
	 * useless, since pts devices are interchangeable, and we
	 * do not know if the specific number will be available at
	 * restore time. We therefore use a struct slspts instead
	 * of a name to represent it.
	 */
	if (slsckpt_vnode_ckptbyname(vp)) {
		/* Get the location of the node in the VFS/SLSFS. */
		error = slsckpt_vnode(vp, sckpt_data);
		if (error != 0)
			return (error);

		return (0);
	}

	/* If the vnode has no name and isn't a tty we can't checkpoint. */
	if (!slsckpt_vnode_istty(vp))
		return (EINVAL);

	/* Otherwise we checkpoint the tty slave. */
	error = slspts_checkpoint_vnode(vp, sb);
	if (error != 0)
		return (error);

	return (0);
}

static int
slsvn_restore(void *slsbacker, struct slsfile *finfo,
    struct slsrest_data *restdata, struct file **fpp)
{
	struct thread *td = curthread;
	struct vnode *vp;
	struct file *fp;
	int error;

	error = slskv_find(
	    restdata->sckpt->sckpt_vntable, finfo->vnode, (uintptr_t *)&vp);
	if (error != 0) {
		/* XXX Ignore the error if ignoring unlinked files. */
		DEBUG("Probably restoring an unlinked file");
		return (error);
	}

	/* Create the file handle, open the vnode associate the two. */
	error = falloc_noinstall(td, &fp);
	if (error != 0)
		return (error);

	VOP_LOCK(vp, LK_EXCLUSIVE);
	error = vn_open_vnode(vp, finfo->flag, td->td_ucred, td, fp);
	if (error != 0) {
		VOP_UNLOCK(vp, 0);
		fdrop(fp, td);
		return (error);
	}

	vref(vp);
	fp->f_vnode = vp;

	/* Manually attach the vnode method vector. */
	if (fp->f_ops == &badfileops) {
		fp->f_seqcount = 1;
		finit(fp, finfo->flag, DTYPE_VNODE, vp, &vnops);
	}
	VOP_UNLOCK(vp, 0);

	*fpp = fp;

	return (0);
}

/* List of paths for device vnodes that can be included in a checkpoint. */
char *sls_accepted_devices[] = {
	"/dev/null",
	"/dev/zero",
	"/dev/hpet0",
	"/dev/random",
	"/dev/urandom",
	NULL,
};

static bool
slsckpt_is_accepted_device(struct vnode *vp)
{
	struct thread *td = curthread;
	char *freepath = NULL;
	char *fullpath = "";
	bool accepted;
	int error;
	int i;

	/* Get the path of the vnode. */
	error = vn_fullpath(td, vp, &fullpath, &freepath);
	if (error != 0) {
		DEBUG1("No path for device vnode %p", vp);
		accepted = false;
		goto out;
	}

	for (i = 0; sls_accepted_devices[i] != NULL; i++) {
		if (strncmp(fullpath, sls_accepted_devices[i], PATH_MAX) != 0) {
			accepted = true;
			goto out;
		}
	}
	accepted = false;

out:
	free(freepath, M_TEMP);
	return (accepted);
}

static bool
slsvn_supported(struct file *fp)
{
	/* FIFOs are always fine.*/
	if (fp->f_type == DTYPE_FIFO)
		return (true);

	/*
	 * An exception to the "only handle regular files" rule.
	 * Used to handle slave PTYs, as mentioned in slsckpt_file().
	 */
	if ((fp->f_vnode->v_type == VCHR) &&
	    ((fp->f_vnode->v_rdev->si_devsw->d_flags & D_TTY) != 0))
		return (true);

	if (fp->f_vnode->v_type == VCHR)
		return (slsckpt_is_accepted_device(fp->f_vnode));

	/* Handle only regular vnodes for now. */
	if (fp->f_vnode->v_type != VREG)
		return (false);

	return (true);
}

struct slsfile_ops slsvn_ops = {
	.slsfile_supported = slsvn_supported,
	.slsfile_slsid = slsvn_slsid,
	.slsfile_checkpoint = slsvn_checkpoint,
	.slsfile_restore = slsvn_restore,
};
