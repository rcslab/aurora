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
#include "sls.h"
#include "sls_path.h"
#include "sls_file.h"
#include <sls_data.h>


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
	    goto sls_vn_to_path_error; 

	len = strnlen(fullpath, PATH_MAX);
	error = sbuf_bcpy(sb, fullpath, len);
	if (error != 0)
	    goto sls_vn_to_path_error;

	sbuf_finish(sb);

	free(freepath, M_TEMP);
	*sbp = sb;

	return 0;

sls_vn_to_path_error:

	sbuf_delete(sb);
	free(freepath, M_TEMP);

	return error;
}

int 
sls_vn_to_path_append(struct vnode *vp, struct sbuf *sb)
{
	int magic = SLS_STRING_MAGIC;
	struct sbuf *path;
	char *data;
	size_t len;
	int error = 0;

	error = sls_vn_to_path(vp, &path);
	if (error != 0)
	    return error;

	error = sbuf_bcat(sb, (void *) &(magic), sizeof(magic));
	if (error != 0)
	    goto sls_vn_to_path_append_done;

	len = sbuf_len(path);
	error = sbuf_bcat(sb, (void *) &len, sizeof(len));
	if (error != 0)
	    goto sls_vn_to_path_append_done;

	data = sbuf_data(path);
	error = sbuf_bcat(sb, data, len);
	if (error != 0) 
	    goto sls_vn_to_path_append_done;

sls_vn_to_path_append_done:
	sbuf_delete(path);

	return error;
}

/*
 * Returns the vnode associated with the filename in the sbuf.
 */
int
sls_path_to_vn(struct sbuf *sb, struct vnode **vpp)
{
	struct nameidata backing_file;
	char *path;
	int error;

	path = sbuf_data(sb);

	NDINIT(&backing_file, LOOKUP, FOLLOW, UIO_SYSSPACE, path, curthread);
	error = namei(&backing_file);
	if (error != 0)
	    return error;

	*vpp = backing_file.ni_vp;

	/* It's a no-op I think, since we don't pass SAVENAME */
	NDFREE(&backing_file, NDF_ONLY_PNBUF);

	return 0;
}


/*
 * Stores the path
 */
int
sls_store_path(struct sbuf *sb, int fd) 
{
    int error;
    char *path;
    size_t len;

    path = sbuf_data(sb);
    len = sbuf_len(sb);
    if (len < 0)
	return -1;

    error = sls_fd_write(&len, sizeof(len), fd);
    if (error != 0)
	return error;

    error = sls_fd_write(path, len, fd);
    if (error != 0)
	return error;

    return 0;
}

int
sls_load_path(struct sbuf **sbp, struct file *fp) 
{
    int error;
    size_t len;
    char *path = NULL;
    int magic;
    struct sbuf *sb = NULL;


    error = sls_file_read(&magic, sizeof(magic), fp);
    if (error != 0)
	return error;

    if (magic != SLS_STRING_MAGIC)
	return EINVAL;

    error = sls_file_read(&len, sizeof(len), fp);
    if (error != 0)
	return error;

    /* First copy the data into a temporary raw buffer */
    path = malloc(len + 1, M_SLSMM, M_WAITOK);
    error = sls_file_read(path, len, fp);
    if (error != 0)
	goto load_path_error;
    path[len++] = '\0';

    sb = sbuf_new_auto();
    if (sb == NULL)
	goto load_path_error;

    /* Then move it over to the sbuf */
    error = sbuf_bcpy(sb, path, len);
    if (error != 0)
	goto load_path_error;

    error = sbuf_finish(sb);
    if (error != 0)
	goto load_path_error;

    *sbp = sb;
    free(path, M_SLSMM);

    return 0;

load_path_error:

    if (sb != NULL)
	sbuf_delete(sb);

    free(path, M_SLSMM);
    *sbp = NULL;
    return error;

}
