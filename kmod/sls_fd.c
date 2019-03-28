#include <sys/param.h>

#include <machine/param.h>

#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/shm.h>
#include <sys/syscallsubr.h>
#include <sys/vnode.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "slsmm.h"
#include "path.h"
#include "sls_data.h"
#include "sls_fd.h"

/* XXX Remove when we get the custom kernel */
#include "copied_fd.h"

static int
file_checkpoint(struct proc *p, struct file *file, struct file_info *info, int fd)
{
	int error;
	char *filename = NULL;
	size_t filename_len = 0;

	PROC_UNLOCK(p);
	error = vnode_to_filename(file->f_vnode, &filename, &filename_len);
	PROC_LOCK(p);
	if (error) {
	    printf("error: vnode_to_filename failed with code %d\n", error);
	    return error;
	}

	info->filename = filename;
	info->filename_len = filename_len;

	/* TODO Handle all other kinds of file descriptors, not just vnodes */
	info->type = file->f_type;
	info->flag = file->f_flag;
	info->offset = file->f_offset;
	info->fd = fd;

	info->magic = SLS_FILE_INFO_MAGIC;

	return 0;
}


/* Roughly follow the kern_openat syscall */
static int
file_restore(struct proc *p, struct filedesc *filedesc, struct file_info *info)
{
	struct nameidata backing_file;
	struct file *file;
	struct vnode *vp __unused;
	struct thread *td;
	int flags;
	int cmode;
	int error = 0;

	td = TAILQ_FIRST(&p->p_threads);

	error = falloc_noinstall(td, &file);
	if (error)
	    return error;

	PROC_UNLOCK(p);
	FILEDESC_XUNLOCK(filedesc);
	NDINIT(&backing_file, LOOKUP, FOLLOW, UIO_SYSSPACE, info->filename, td);

	flags = info->flag;
	cmode = VREAD | VWRITE | VAPPEND;
	/*
	* XXX The cmode flags are hardcoded. This will be fixed when we add
	* the interposition layer and will be able to
	* use the kern_openat() call instead of manually opening the vnode.
	*/
	error = vn_open(&backing_file, &flags, cmode, file);
	NDFREE(&backing_file, NDF_ONLY_PNBUF);

	FILEDESC_XLOCK(filedesc);
	PROC_LOCK(p);
	if (error) {
	    printf("Error: vn_open failed with %d\n", error);
	    return error;
	}


	file->f_flag = flags;
	file->f_type = info->type;
	file->f_vnode = backing_file.ni_vp;
	file->f_offset = info->offset;

	VOP_UNLOCK(backing_file.ni_vp, 0);

	/* Flags arg is zero because we don't care about close on exec rn */
	fdused(p->p_fd, info->fd);
	_finstall(filedesc, file, info->fd, 0, NULL);

	if (file->f_ops == &badfileops) {
	    file->f_seqcount = 1;
	    finit(file, (flags & FMASK) | (file->f_flag & FHASLOCK),
		    DTYPE_VNODE, file->f_vnode, &vnops);
	}

	return 0;
}

int
fd_checkpoint(struct proc *p, struct filedesc_info *filedesc_info)
{
	int i;
	char *cdir_path = NULL, *rdir_path = NULL;
	size_t cdir_len, rdir_len;
	int error = 0;
	struct file_info *infos;
	struct vnode *cdir, *rdir;
	struct file *fp;
	int index;
	struct filedesc *filedesc;

	filedesc = p->p_fd;

	cdir = filedesc->fd_cdir;
	rdir = filedesc->fd_rdir;

	vhold(cdir);
	vhold(rdir);

	infos = filedesc_info->infos;
	FILEDESC_XLOCK(filedesc);

	PROC_UNLOCK(p);
	error = vnode_to_filename(cdir, &cdir_path, &cdir_len);
	PROC_LOCK(p);
	if (error) {
	    printf("Error: cdir vnode_to_filename failed with code %d\n", error);
	    goto fd_checkpoint_error;
	}
	filedesc_info->cdir= cdir_path;
	filedesc_info->cdir_len = cdir_len;


	PROC_UNLOCK(p);
	error = vnode_to_filename(rdir, &rdir_path, &rdir_len);
	PROC_LOCK(p);
	if (error) {
	    printf("Error: rdir vnode_to_filename failed with code %d\n", error);
	    goto fd_checkpoint_error;
	}
	filedesc_info->rdir= rdir_path;
	filedesc_info->rdir_len = rdir_len;

	filedesc_info->fd_cmask = filedesc->fd_cmask;
	filedesc_info->num_files = 0;
	for (i = 0; i <= filedesc->fd_lastfile; i++) {
	    if (!fdisused(filedesc, i)) {
		printf("FD %d unused\n", i);
		continue;
	    }

	    fp = filedesc->fd_files->fdt_ofiles[i].fde_file;

	    /* We only handle vnodes right now */
	    if (fp->f_type != DTYPE_VNODE ||
		    fp->f_vnode->v_type != VREG)
		continue;


	    index = filedesc_info->num_files;

	    if(!file_checkpoint(p, fp, &infos[index], i))
		filedesc_info->num_files++;
	    else
		printf("file checkpointing for %d failed\n", i);
	}

	vdrop(cdir);
	vdrop(rdir);

	FILEDESC_XUNLOCK(filedesc);

	filedesc_info->magic = SLS_FILEDESC_INFO_MAGIC;


	return 0;
fd_checkpoint_error:

	vdrop(cdir);
	vdrop(rdir);

	FILEDESC_XUNLOCK(filedesc);

	free(cdir_path, M_SLSMM);
	free(rdir_path, M_SLSMM);

	return error;
}

int
fd_restore(struct proc *p, struct filedesc_info *info)
{
	struct vnode *cdir, *rdir;
	struct vnode *old_cdir, *old_rdir;
	int error = 0;
	int i;
	struct file_info *infos = info->infos;
	struct thread *td; 
	struct filedesc *fdp;

	td = TAILQ_FIRST(&p->p_threads);
	fdp = p->p_fd;
	
	PROC_UNLOCK(p);

	error = filename_to_vnode(info->cdir, &cdir);
	if (error) {
	    printf("Error: filename_to_vnode failed with %d\n", error);
	    return error;
	}

	error = filename_to_vnode(info->rdir, &rdir);
	if (error) {
	    printf("Error: filename_to_vnode failed with %d\n", error);
	    return error;
	}

	PROC_LOCK(p);

	/* XXX Keep stdin/stdout/stderr open? */
	fdunshare(td);
	fdcloseexec(td);

	FILEDESC_XLOCK(fdp);

	old_cdir = fdp->fd_cdir;
	old_rdir = fdp->fd_rdir;

	vrefact(cdir);
	vrefact(rdir);

	fdp->fd_cdir = cdir;
	fdp->fd_rdir = rdir;

	vrele(old_cdir);
	vrele(old_rdir);

	fdp->fd_cmask = info->fd_cmask;

	/* XXX close previous files */
	for (i = 0; i < info->num_files; i++) {
	    file_restore(p, fdp, &infos[i]);
	}

	FILEDESC_XUNLOCK(fdp);

	return 0;
}
