#include <sys/types.h>
#include <sys/param.h>
#include <sys/queue.h>
#include <vm/uma.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/unistd.h>

#include <machine/atomic.h>

#include "slos.h"
#include "slsfs.h"
#include "slsfs_subr.h"

uint64_t pids;

int 
slsfs_init(struct vfsconf *vfsp)
{
    pids = SLOS_ROOT_INODE + 1;
    return (0);
}

int 
slsfs_uninit(struct vfsconf *vfsp)
{
    return (0);
}

int
slsfs_newnode(struct slos *slos, uint16_t mode, uint64_t *ppid, struct slos_node **spp)
{
    //struct slos_node *vp;
    uint64_t pid;
    int error;

    if (*ppid != 0) {
	pid = atomic_fetchadd_64(&pids, 1);
    } else {
	pid = *ppid;
    }

    error = slos_icreate(slos, pid, mode);
    if (error) {
	*ppid = 0;
	return (error);
    }

    *ppid = pid;
    return slsfs_getnode(slos, *ppid, spp);
}


int
slsfs_getnode(struct slos *slos, uint64_t pid, struct slos_node **spp)
{
    struct slos_node *vp;

    vp = slos_iopen(slos, pid);
    if (vp != NULL) {
	*spp = vp;
	return (0);
    }

    return (EINVAL);
}
