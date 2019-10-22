#include <sys/types.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/dirent.h>

#include "slsfs.h"
#include "slos_io.h"
#include "slos_inode.h"
#include "slos_record.h"
#include "slsfs_dir.h"

int 
slsfs_add_dirent(struct slos_node *vp, uint64_t ino, char *nameptr, long namelen, uint8_t type)
{
    struct dirent dir;
    struct uio auio;
    struct iovec aiov;
    uint64_t rno;
    int error;

    uint64_t blksize = SVPBLK(vp);

    /* Create the SLOS record for the metadata. */
    error = slos_rcreate(vp, type, &rno);
    if (error != 0) {
	DBUG("Problem creating record in adding directory entry\n");
	return error;
    }

    /* Create the UIO for the disk. */
    dir.d_fileno = ino;
    dir.d_off = rno * blksize;
    memcpy(&dir.d_name, nameptr, namelen);
    dir.d_type = type;
    dir.d_reclen = blksize;

    aiov.iov_base = &dir;
    aiov.iov_len = sizeof(struct dirent);
    slos_uioinit(&auio, 0, UIO_WRITE, &aiov, 1);
    error = slos_rwrite(vp, rno, &auio);
    if (error) {
	DBUG("Problem writing record in adding directory entry\n");
	return (error);
    }

    return (0);
}

static int
slsfs_init_dir(struct slos_node *vp, uint64_t ino, uint64_t parent) 
{
    /* Create the pointer to current directory */
    if (slsfs_add_dirent(vp, parent, "..", 2, DT_DIR) ||
	    slsfs_add_dirent(vp, ino, ".", 1, DT_DIR)) {
	DBUG("Problem initing dir %lu\n", ino);

	return (-1);
    }
    vp->vno_ino->ino_link_num = 2;
    DBUG("Init dir done\n");

    return (0);
}

/* Current problem:
 * Because we constantly have these write to disk operations when errors do occur, we now
 * have to go back and clean up the disk we just used, we need to have it so writes are buffered 
 * rather than constantly being executed. This is where the bufcache comes in handy we need to have 
 * some sort of option to allow for straight flush to disk or just buffered writes. */
int
slsfs_create_dir(struct slos *slos, uint64_t ino, uint64_t parent_ino, struct slos_node **vpp)
{
    mode_t mode;
    struct slos_node *vp;
    int error;

    mode = MAKEIMODE(VDIR, S_IRWXU | S_IRWXG | S_IRWXO);
    error = slos_icreate(slos, ino, mode);
    if (error) {
	return (error);
    }

    // Open the vnode
    vp = slos_iopen(slos, ino);
    error = slsfs_init_dir(vp, ino, parent_ino);
    if (error) {
	return (error);
    }

    *vpp = vp;
    return (0);
}
