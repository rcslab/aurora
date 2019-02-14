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
#include <sys/vnode.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include "_slsmm.h"
#include "path.h"

int
vnode_to_filename(struct vnode *vp, char **path, size_t *len)
{
	char *filename;
	char *freebuf = NULL;
	char *retbuf = "error";
	int filename_len;
	int error;

	filename = malloc(PATH_MAX, M_SLSMM, M_NOWAIT);
	if (filename == NULL) {
	    *path = NULL;
	    return ENOMEM;
	}

	vref(vp);
	error = vn_fullpath(curthread, vp, &retbuf, &freebuf);
	vrele(vp);
	if (error != 0) {
	    printf("vn_fullpath failed: error %d\n", error);
	    free(filename, M_SLSMM);
	    return error; 
	}

	/* 
	* If this seems weird, it's because that's how vn_fullpath is supposed
	* to work. 
	*/
	filename_len = strnlen(retbuf, PATH_MAX);
	strncpy(filename, retbuf, filename_len);
	filename[filename_len++] = '\0';
	free(freebuf, M_TEMP);

	*path = filename;
	*len = filename_len;
	printf("Length: %lu, path: %s\n", *len, *path);

	return 0;
}

int
filename_to_vnode(char *path, struct vnode **vpp)
{
	struct nameidata backing_file;
	int error;

	NDINIT(&backing_file, LOOKUP, FOLLOW, UIO_SYSSPACE, path, curthread);
	error = namei(&backing_file);
	if (error) {
	    printf("Error: namei for path failed with %d\n", error);
	    return error;
	}
	*vpp = backing_file.ni_vp;

	/* It's a no-op I think, since we don't pass SAVENAME */
	NDFREE(&backing_file, NDF_ONLY_PNBUF);

	return 0;
}
