#include <sys/param.h>

#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include "slos_alloc.h"
#include "slos_internal.h"
#include "slos_inode.h"
#include "slos_bootalloc.h"
#include "slos_btree.h"
#include "slos_io.h"
#include "slos_record.h"
#include "slosmm.h"

MALLOC_DEFINE(M_SLOS, "slos", "SLOS");

/*
 * Returns the vnode associated with the filename in the sbuf.
 */
static int
slos_path_to_vnode(char *path, struct vnode **vpp)
{
	struct nameidata backing_file;
	int error;

	NDINIT(&backing_file, LOOKUP, FOLLOW, UIO_SYSSPACE, path, curthread);
	error = namei(&backing_file);
	if (error != 0)
	    return error;

	*vpp = backing_file.ni_vp;

	/* It's a no-op I think, since we don't pass SAVENAME */
	NDFREE(&backing_file, NDF_ONLY_PNBUF);

	return 0;
}

static int
slos_itreeopen(struct slos *slos)
{
	uint64_t itree;

	itree = slos->slos_sb->sb_inodes.offset;
	slos->slos_inodes = btree_init(slos, itree, ALLOCMAIN);
	if (slos->slos_inodes == NULL)
	    return EIO;

	return 0;
}

static void
slos_itreeclose(struct slos *slos)
{
	btree_discardelem(slos->slos_inodes);
	btree_destroy(slos->slos_inodes);
}
