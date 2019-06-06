#include <sys/param.h>

#include <machine/param.h>

#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
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
#include "sls_path.h"
#include "sls_data.h"
#include "sls_fd.h"
#include "sls.h"

/* XXX Remove when we get the custom kernel */
#include "imported_sls.h"

static int
file_ckpt(struct proc *p, struct file *file, int fd, struct sbuf *sb)
{
	int error;
	struct file_info info;

	/* TODO Handle all other kinds of file descriptors, not just vnodes */
	info.type = file->f_type;
	info.flag = file->f_flag;
	info.offset = file->f_offset;
	info.fd = fd;

	info.magic = SLS_FILE_INFO_MAGIC;

	error = sbuf_bcat(sb, (void *) &info, sizeof(info));
	if (error != 0)
	    return error;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(file->f_vnode, sb);
	PROC_LOCK(p);
	if (error != 0)
	    return error;


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
fd_ckpt(struct proc *p, struct sbuf *sb)
{
	int i;
	int error = 0;
	struct file *fp;
	struct filedesc *filedesc;
	struct filedesc_info filedesc_info;
	struct file_info sentinel;

	filedesc = p->p_fd;

	vhold(filedesc->fd_cdir);
	vhold(filedesc->fd_rdir);

	filedesc_info.fd_cmask = filedesc->fd_cmask;
	filedesc_info.num_files = 0;
	filedesc_info.magic = SLS_FILEDESC_INFO_MAGIC;

	FILEDESC_XLOCK(filedesc);

	error = sbuf_bcat(sb, (void *) &filedesc_info, sizeof(filedesc_info));
	if (error != 0)
	    goto fd_ckpt_done;

	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(filedesc->fd_cdir, sb);
	PROC_LOCK(p);
	if (error) {
	    SLS_DBG("Error: cdir sls_vn_to_path failed with code %d\n", error);
	    goto fd_ckpt_done;
	}


	PROC_UNLOCK(p);
	error = sls_vn_to_path_append(filedesc->fd_rdir, sb);
	PROC_LOCK(p);
	if (error) {
	    SLS_DBG("Error: rdir sls_vn_to_path failed with code %d\n", error);
	    goto fd_ckpt_done;
	}


	for (i = 0; i <= filedesc->fd_lastfile; i++) {
	    if (!fdisused(filedesc, i))
		continue;

	    fp = filedesc->fd_files->fdt_ofiles[i].fde_file;

	    /* We only handle vnodes right now */
	    if (fp->f_type != DTYPE_VNODE || fp->f_vnode->v_type != VREG)
		continue;

	    error = file_ckpt(p, fp, i, sb);
	    if (error != 0)
		goto fd_ckpt_done;
	}

	memset(&sentinel, 0, sizeof(sentinel));
	sentinel.magic = SLS_FILES_END;
	error = sbuf_bcat(sb, (void *) &sentinel, sizeof(sentinel));
	if (error != 0)
	    goto fd_ckpt_done;


fd_ckpt_done:

	vdrop(filedesc->fd_cdir);
	vdrop(filedesc->fd_rdir);

	FILEDESC_XUNLOCK(filedesc);


	return error;
}

int
fd_rest(struct proc *p, struct filedesc_info info)
{
	struct file_info *infos = info.infos;
	int stdfds[] = { 0, 1, 2 };
	struct filedesc *newfdp;
	char *cdir, *rdir;
	int error = 0;
	int i;

	cdir = sbuf_data(info.cdir);
	rdir = sbuf_data(info.rdir);
	/* 
	 * We assume that fds 0, 1, and 2 are still stdin, stdout, stderr.
	 * This is a valid assumption considering that we control the process 
	 * on top of which we restore.
	 */
	error = fdcopy_remapped(p->p_fd, stdfds, 3, &newfdp);
	if (error != 0)
	    return error;
	fdinstall_remapped(curthread, newfdp);

	PROC_UNLOCK(p);
	error = kern_chdir(curthread, cdir, UIO_SYSSPACE);
	if (error != 0)
	    return error;

	error = kern_chroot(curthread, rdir, UIO_SYSSPACE);
	if (error != 0)
	    return error;
	PROC_LOCK(p);

	FILEDESC_XLOCK(newfdp);
	newfdp->fd_cmask = info.fd_cmask;
	FILEDESC_XUNLOCK(newfdp);

	for (i = 0; i < info.num_files; i++) {
	    error = file_rest(p, &infos[i]);
	    if (error != 0)
		return error;
	}


	return 0;
}
