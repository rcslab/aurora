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

/* We have only one SLOS currently, make it global. */
struct slos slos;

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

/*
 * Get the vnode corresponding to the disk device of the given name. 
 */
static int
slos_vpopen(char *path)
{
	struct nameidata nd;
	int oflags;
	int error;

	NDINIT(&nd, LOOKUP, NOFOLLOW, UIO_SYSSPACE, path, curthread);
	oflags = FREAD | FWRITE;

	error = vn_open_cred(&nd, &oflags, S_IRWXU | S_IRWXO, 
		0, curthread->td_proc->p_ucred, NULL); 
	if (error != 0)
	    return error;

	NDFREE(&nd, NDF_ONLY_PNBUF);

	if (error != 0) {
	    printf("ERROR: Could not find SLOS\n");
	    return error;
	}

	VOP_UNLOCK(nd.ni_vp, 0);
	    
	slos.slos_vp = nd.ni_vp;

	return 0;
}

/*
 * Close the SLOS' vnode.
 */
static int
slos_vpclose(void)
{
	int error;

	error = vn_close(slos.slos_vp, FREAD | FWRITE, 
		curthread->td_proc->p_ucred, curthread);

	slos.slos_vp = NULL;
	
	return error;
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


static int
slosHandler(struct module *inModule, int inEvent, void *inArg) {
	int error = 0;

	switch (inEvent) {
	    case MOD_LOAD:
		bzero(&slos, sizeof(slos));

		/* Set up the global SLOS mutex. */
		mtx_init(&slos.slos_mtx, "slosmtx", NULL, MTX_DEF);

		/* Open the device for writing. */
		error = slos_path_to_vnode("/dev/vtbd1", &slos.slos_vp);
		if (error != 0)
		    return error;

		/* 
		 * Open the consumer, associate the vnode's bufobj with it. 
		 * Only needed if we actually use a device for the filesystem.
		 */
		if (slos.slos_vp->v_type == VCHR) {
		    g_topology_lock();
		    error = g_vfs_open(slos.slos_vp, &slos.slos_cp, "slos", 1);
		    g_topology_unlock();
		    if (error != 0)
			return error;
		}
		
		/* Read in the superblock. */
		error = slos_sbread();
		if (error != 0) {
		    printf("ERROR: slos_sbread failed with %d\n", error);
		    return error;
		}
		
		/* Set up the bootstrap allocator. */
		error = slos_bootinit(&slos);
		if (error != 0) {
		    printf("slos_bootinit had error %d\n", error);
		    return error;
		}
		    
		slos.slos_alloc = slos_alloc_init(&slos);
		if (slos.slos_alloc == NULL) {
		    printf("ERROR: slos_alloc_init failed to set up allocator\n");
		    return EINVAL;
		}

		/* Get the btree of inodes in the SLOS. */
		error = slos_itreeopen(&slos);
		if (error != 0) {
		    printf("ERROR: slos_itreeopen failed with %d\n", error);
		    return error;
		}

		/* Set upa the open vnode hashtable. */
		error = slos_vhtable_init(&slos);
		if (error != 0) {
		    printf("ERROR: slos_vhtable_init failed with %d\n", error);
		    return error;
		}


		printf("SLOS Loaded.\n");

		break;
	    case MOD_UNLOAD:

		/* 
		 * XXX Flush all in-memory blocks? 
		 * Maybe geom takes care of that? 
		 */

		mtx_lock(&slos.slos_mtx);

		/* Remove the vnodes table. */
		if (slos.slos_vhtable != NULL) {
		    error = slos_vhtable_fini(&slos);
		    /* If we have open vnodes, we can't unmount yet. */
		    if (error != 0) {
			printf("ERROR: slos_vhtable_fini failed with %d\n", error);
			mtx_unlock(&slos.slos_mtx);
			return error;
		    }
		}

		/* 
		 * XXX When the hooks to SLS are built,
		 * we need to have a way to remove all references
		 * to the SLOS, so that no one is able to make
		 * request while/after we unload. Whatever that
		 * is, it has to be done here.
		 */

		/* Flush the inodes btree from memory. */
		if (slos.slos_inodes != NULL)
		    slos_itreeclose(&slos);


		/* Tear down the main allocator. */
		slos_alloc_destroy(&slos);

		/* Tear down the boot allocator. */
		if (slos.slos_bootalloc != NULL)
		    slos_bootdestroy(&slos);

		/* Free the superblock. */
		free(slos.slos_sb, M_SLOS);

		/* Close the geom disk consumer. */
		if (slos.slos_cp != NULL) {
		    g_topology_lock();
		    g_vfs_close(slos.slos_cp);
		    g_topology_unlock();
		}

		/* Close the devfs vnode. */
		if (slos.slos_vp != NULL) {
		    error = slos_vpclose();
		    if (error != 0)
			printf("ERROR: slos_vpclose() failed with %d\n", error);
		}

		mtx_unlock(&slos.slos_mtx);

		/* Destroy the mutex. */
		mtx_destroy(&slos.slos_mtx);

		printf("SLOS Unloaded.\n");
		break;
	    default:
		error = EOPNOTSUPP;
		break;
	}

	return error;
}

static moduledata_t moduleData = {
	"slos",
	slosHandler,
	NULL
};

DECLARE_MODULE(slos, moduleData, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(slos, 0);
