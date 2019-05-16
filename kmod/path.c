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

int
sls_vn_to_path(struct vnode *vp, struct sbuf **sbp)
{
	char *freebuf = NULL;
	char *retbuf = "error";
	struct sbuf *sb;
	int len;
	int error;

	sb = sbuf_new_auto();
	if (sb == NULL)
	    return ENOMEM;

	vref(vp);
	error = vn_fullpath(curthread, vp, &retbuf, &freebuf);
	vrele(vp);
	if (error != 0)
	    goto vnode_to_filename_error; 

	len = strnlen(retbuf, PATH_MAX);
	error = sbuf_bcpy(sb, retbuf, len);
	if (error != 0)
	    goto vnode_to_filename_error;

	error = sbuf_bcat(sb, "\0", 1);
	if (error != 0)
	    goto vnode_to_filename_error;

	error = sbuf_finish(sb);
	if (error != 0)
	    goto vnode_to_filename_error;

	*sbp = sb;

	free(freebuf, M_TEMP);

	return 0;

vnode_to_filename_error:

	sbuf_delete(sb);
	free(freebuf, M_TEMP);

	return error;
}

int
sls_path_to_vn(struct sbuf *sb, struct vnode **vpp)
{
	struct nameidata backing_file;
	char *path;
	int error;

	path = sbuf_data(sb);
	printf("path: %s\n", path);

	NDINIT(&backing_file, LOOKUP, FOLLOW, UIO_SYSSPACE, path, curthread);
	error = namei(&backing_file);
	if (error != 0)
	    return error;

	*vpp = backing_file.ni_vp;

	/* It's a no-op I think, since we don't pass SAVENAME */
	NDFREE(&backing_file, NDF_ONLY_PNBUF);

	return 0;
}
