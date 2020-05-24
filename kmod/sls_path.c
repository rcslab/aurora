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

#include <sls_data.h>

#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_path.h"

/*
 * Append the filename of a vnode to a given raw buffer.
 */
int
sls_vn_to_path_raw(struct vnode *vp, char *buf)
{
	char *freepath = NULL;
	char *fullpath = "";
	struct sbuf *sb;
	int error;

	sb = sbuf_new_auto();

	vref(vp);
	error = vn_fullpath(curthread, vp, &fullpath, &freepath);
	vrele(vp);
	if (error != 0) {
		free(freepath, M_TEMP);
		return (error);
	}

	memcpy(buf, fullpath, PATH_MAX);
	free(freepath, M_TEMP);

	return (0);
}


/*
 * Appends the filename of the given vnode to the sbuf.
 */
int
sls_vn_to_path(struct vnode *vp, struct sbuf **sbp)
{
	char *freepath = NULL;
	char *fullpath = "";
	struct sbuf *sb;
	size_t len;
	int error;

	sb = sbuf_new_auto();

	vref(vp);
	error = vn_fullpath(curthread, vp, &fullpath, &freepath);
	vrele(vp);
	if (error != 0)
		goto error;

	len = strnlen(fullpath, PATH_MAX);
	error = sbuf_bcpy(sb, fullpath, len);
	if (error != 0)
		goto error;

	sbuf_finish(sb);

	free(freepath, M_TEMP);
	*sbp = sb;

	return 0;

error:

	sbuf_delete(sb);
	free(freepath, M_TEMP);

	return error;
}

int
sls_path_append(const char *data, size_t len, struct sbuf *sb)
{
	uint64_t magic = SLSSTRING_ID;
	int error; 

	error = sbuf_bcat(sb, (void *) &(magic), sizeof(magic));
	if (error != 0)
		return error;

	error = sbuf_bcat(sb, (void *) &len, sizeof(len));
	if (error != 0)
		return error;

	error = sbuf_bcat(sb, data, len);
	if (error != 0) 
		return error;


	return 0;
}

int 
sls_vn_to_path_append(struct vnode *vp, struct sbuf *sb)
{
	struct sbuf *path;
	int error = 0;
	char *data;
	size_t len;

	error = sls_vn_to_path(vp, &path);
	if (error != 0)
		return error;

	data = sbuf_data(path);
	len = sbuf_len(path);

	error = sls_path_append(data, len, sb);
	/* No matter whether it succeeded, the cleanup is the same. */

	sbuf_delete(path);

	return (error);
}

/*
 * Returns the vnode associated with the filename in a raw buf.
 */
int
sls_path_to_vn_raw(char *buf, struct vnode **vpp)
{
	struct nameidata backing_file;
	int error;

	NDINIT(&backing_file, LOOKUP, FOLLOW, UIO_SYSSPACE, buf, curthread);
	error = namei(&backing_file);
	if (error != 0)
		return error;

	*vpp = backing_file.ni_vp;

	/* It's a no-op I think, since we don't pass SAVENAME */
	NDFREE(&backing_file, NDF_ONLY_PNBUF);

	return (0);

}

/*
 * Returns the vnode associated with the filename in the sbuf.
 */
int
sls_path_to_vn(struct sbuf *sb, struct vnode **vpp)
{
	return (sls_path_to_vn_raw(sbuf_data(sb), vpp));
}


