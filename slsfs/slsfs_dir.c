#include <sys/types.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/dirent.h>
#include <sys/namei.h>

#include <slsfs.h>
#include <slos_inode.h>
#include <slos_record.h>
#include <slos_io.h>

#include "../slos/slosmm.h"

#include "slsfs_dir.h"
#include "slsfs_subr.h"

uint64_t
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
    if (error) {
	DBUG("Problem creating record for directory\n");
	return -1;
    }

    memset(&dir, 0, sizeof(struct dirent));
    dir.d_fileno = ino;
    dir.d_off = rno * blksize;
    memcpy(&dir.d_name, nameptr, namelen);
    dir.d_type = type;
    dir.d_reclen = blksize;
    dir.d_namlen = namelen;


    aiov.iov_base = &dir;
    aiov.iov_len = sizeof(struct dirent);
    /* Create the UIO for the disk. */
    slos_uioinit(&auio, 0, UIO_WRITE, &aiov, 1);

    /* Write the record */
    error = slos_rwrite(vp, rno, &auio);
    if (error){
	DBUG("Problem writing record to node\n");

	return (-1);
    }
    
    SLSINO(vp)->ino_link_num++;
    vp->sn_ino->ino_size = SLSINO(vp)->ino_link_num * blksize;
    /* Update inode size to disk */
    error = slos_iupdate(vp);
    if (error) { 
	DBUG("Problem updating node\n");

	return (-1);
    }

    return (rno);
}

int
slsfs_init_dir(struct slos_node *dvp, struct slos_node *vp, struct componentname *name) 
{
    /* Create the pointer to current directory */
    DBUG("Adding ..\n");
    uint64_t rno;
    if (slsfs_add_dirent(vp, SVINUM(dvp), "..", 2, DT_DIR) == -1) {
	DBUG("Problem adding ..\n");
	return (-1);
    }

    DBUG("Adding .\n");
    if (slsfs_add_dirent(vp, SVINUM(vp), ".", 1, DT_DIR) == -1) {
	DBUG("Problem adding .\n");
	return (-1);
    }

    DBUG("Adding %s\n", name->cn_nameptr);
    rno = slsfs_add_dirent(dvp, SVINUM(vp), name->cn_nameptr, name->cn_namelen, DT_DIR);
    if (rno == -1) {
	return (-1);
    }

    return (0);
}

/* 
 * Unlink the child file from the parent
 */
int
slsfs_unlink_dir(struct slos_node *dvp, struct slos_node *vp, struct componentname *name)
{
    struct slos_record *record;
    struct dirent dir;
    int error;

    uint64_t blksize = SVPBLK(vp);

    error = slsfs_lookup_name(dvp, name, &dir, &record);
    if (error) {
	return (error);
    }

    /* Remove the record to this directory in the parent directory */
    DBUG("Found dir; removing directory record %s\n", name->cn_nameptr);
    error = slos_rremove(dvp, record); 
    free(record, M_SLOS);
    if (error) {
	return (error);
    }
    
    SLSINO(dvp)->ino_link_num--;
    SLSINO(dvp)->ino_size = SLSINO(dvp)->ino_link_num * blksize;
    /* Update inode size to disk */
    error  = slos_iupdate(dvp);
    if (error) {
	return (error);
    }

    return (0);
}

int
slsfs_lookup_name(struct slos_node *dvp, struct componentname *name, struct dirent *dir_p, struct slos_record **rec) 
{
    struct slos_record *record, *tmp;
    struct uio auio;
    struct iovec aiov;
    int error;

    SLOS_RECORD_FOREACH(dvp, record, tmp, error) {
	struct dirent dir;
	aiov.iov_base = &dir;
	aiov.iov_len = sizeof(dir);
	/* Create the UIO for the disk. */
	slos_uioinit(&auio, 0, UIO_READ, &aiov, 1);
	error = slos_rread(dvp, record->rec_num, &auio);
	if (error) {
	    if (rec != NULL) {
		*rec = record;
	    } else {
		free(record, M_SLOS);
	    }

	    return EIO;
	}
	if (strcmp(name->cn_nameptr, dir.d_name) == 0) {
	    *dir_p = dir;
	    if (rec != NULL) {
		*rec = record;
	    } else {
		free(record, M_SLOS);
	    }

	    return (0);
	}
    }

    return (error);
}
