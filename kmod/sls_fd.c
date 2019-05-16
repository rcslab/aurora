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
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/unistd.h>
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
#include "sls.h"

/* XXX Remove when we get the custom kernel */
#include "imported_sls.h"

static int
file_ckpt(struct proc *p, struct file *file, struct file_info *info, int fd)
{
	int error;

	PROC_UNLOCK(p);
	error = sls_vn_to_path(file->f_vnode, &info->path);
	PROC_LOCK(p);
	if (error != 0)
	    return error;


	/* TODO Handle all other kinds of file descriptors, not just vnodes */
	info->type = file->f_type;
	info->flag = file->f_flag;
	info->offset = file->f_offset;
	info->fd = fd;

	info->magic = SLS_FILE_INFO_MAGIC;

	return 0;
}

static int
file_rest(struct proc *p, struct file_info *info)
{
	char *path;
	int error;

	path = sbuf_data(info->path);

	error = kern_openat(curthread, info->fd, path, 
		UIO_SYSSPACE, O_RDWR | O_CREAT, S_IRWXU);	
	if (error != 0)
	    return error;

	error = kern_lseek(curthread, info->fd, info->offset, SEEK_SET);
	if (error != 0)
	    return error;

	return 0;
}

int
fd_ckpt(struct proc *p, struct filedesc_info *filedesc_info)
{
	int i;
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
	error = sls_vn_to_path(cdir, &filedesc_info->cdir);
	PROC_LOCK(p);
	if (error) {
	    printf("Error: cdir sls_vn_to_path failed with code %d\n", error);
	    goto fd_ckpt_error;
	}


	PROC_UNLOCK(p);
	error = sls_vn_to_path(rdir, &filedesc_info->rdir);
	PROC_LOCK(p);
	if (error) {
	    printf("Error: rdir sls_vn_to_path failed with code %d\n", error);
	    goto fd_ckpt_error;
	}

	filedesc_info->fd_cmask = filedesc->fd_cmask;
	filedesc_info->num_files = 0;
	for (i = 0; i <= filedesc->fd_lastfile; i++) {
	    if (!fdisused(filedesc, i)) {
		continue;
	    }

	    fp = filedesc->fd_files->fdt_ofiles[i].fde_file;

	    /* We only handle vnodes right now */
	    if (fp->f_type != DTYPE_VNODE ||
		    fp->f_vnode->v_type != VREG)
		continue;


	    index = filedesc_info->num_files;

	    if(file_ckpt(p, fp, &infos[index], i) == 0)
		filedesc_info->num_files++;
	    else 
		goto fd_ckpt_error;
	}

	vdrop(cdir);
	vdrop(rdir);

	FILEDESC_XUNLOCK(filedesc);

	filedesc_info->magic = SLS_FILEDESC_INFO_MAGIC;

	return 0;
fd_ckpt_error:

	vdrop(cdir);
	vdrop(rdir);

	FILEDESC_XUNLOCK(filedesc);


	return error;
}

int
fd_rest(struct proc *p, struct filedesc_info *info)
{
	struct file_info *infos = info->infos;
	int stdfds[] = { 0, 1, 2 };
	struct filedesc *newfdp;
	char *cdir, *rdir;
	int error = 0;
	int i;

	cdir = sbuf_data(info->cdir);
	rdir = sbuf_data(info->rdir);
	/* 
	 * We assume that fds 0, 1, and 2 are still stdin, stdout, stderr.
	 * This is a valid assumption considering that we control the process 
	 * on top of which we restore.
	 */
	error = fdcopy_remapped(p->p_fd, stdfds, 3, &newfdp);
	if (error != 0)
	    return error;
	fdinstall_remapped(curthread, newfdp);

	error = kern_chdir(curthread, cdir, UIO_SYSSPACE);
	if (error != 0)
	    return error;

	error = kern_chroot(curthread, rdir, UIO_SYSSPACE);
	if (error != 0)
	    return error;

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info->fd_cmask;
	FILEDESC_XUNLOCK(newfdp);

	for (i = 0; i < info->num_files; i++) {
	    error = file_rest(p, &infos[i]);
	    if (error != 0)
		return error;
	}


	return 0;
}
