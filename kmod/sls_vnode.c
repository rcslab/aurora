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

#include <slos.h>
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_path.h"

#include "imported_sls.h"

/* Get the name of a vnode. This is the only information we need about it. */
static int
slsckpt_path(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	int error;

	error = sls_vn_to_path_append(vp, sb);

	/* 
	 * XXX Make sure the unlinked file case is handled. 
	 */
	if (error == ENOENT)
	    return (0);

	return (0);
}

static int
slsrest_path(struct sbuf *path, struct slsfile *info, int *fdp, int seekable)
{
	char *filepath;
	int error;
	int fd;

	filepath = sbuf_data(path);
	printf("Restoring %s\n", filepath);

	/* XXX Permissions/flags. Also, is O_CREAT reasonable? */
	error = kern_openat(curthread, AT_FDCWD, filepath, 
		UIO_SYSSPACE, O_RDWR, S_IRWXU);	
	if (error != 0)
	    return (error);

	fd = curthread->td_retval[0];

	/* Vnodes are seekable. Fix up the offset here. */
	if (seekable != 0) {
	    error = kern_lseek(curthread, fd, info->offset, SEEK_SET);
	    if (error != 0)
		return (error);
	}

	/* Export the fd to the caller. */
	*fdp = fd;

	return (0);
}


int
slsckpt_vnode(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	return slsckpt_path(p, vp, sb);
}

int
slsrest_vnode(struct sbuf *path, struct slsfile *info, int *fdp)
{
	return slsrest_path(path, info, fdp, 0);
}

int
slsckpt_fifo(struct proc *p, struct vnode *vp, struct sbuf *sb)
{
	return slsckpt_path(p, vp, sb);
}

int
slsrest_fifo(struct sbuf *path, struct slsfile *info, int *fdp)
{
	return slsrest_path(path, info, fdp, 1);
}
