#include <sys/mount.h>
#include <sys/queue.h>

#include "slos_internal.h"

#define DBUG(fmt, ...) do {			    \
    printf("(%s: Line %d) ", __FILE__, __LINE__);   \
    printf(fmt, ##__VA_ARGS__);			    \
    } while (0) 

#define SLOS_ROOT_INODE (100000)
#define TOSMP(mp) ((struct slsfsmount *)(mp->mnt_data))
#define MPTOSLOS(mp) ((TOSMP(mp)->sp_slos))
#define SLSVP(vp) ((struct slos_node *)(vp->v_data))
#define SLSION(svp) ((struct slos_inode *)(svp->vno_ino))
#define SMPSLOS(smp) (mp->sdev->slos)

struct slsfsmount {
    STAILQ_ENTRY(slsfsmount)	sp_next_mount;

    struct mount		*sp_vfs_mount;
    struct slsfs_device		*sp_sdev;
    struct slos			*sp_slos;
    int				sp_ronly;
};

struct slsfs_device {
    struct vnode		*devvp;
    struct mtx			g_mtx; 
    struct g_provider		*gprovider;
    struct g_consumer		*gconsumer;

    uint64_t			refcnt;
    uint64_t			devsize;
    uint64_t			devblocksize;
};

extern struct slos slos;
extern struct vop_vector sls_vnodeops;
