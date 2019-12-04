#include <sys/mount.h>
#include <sys/queue.h>

struct slos;

#ifdef WITH_DEBUG
#define DBUG(fmt, ...) do {			    \
    printf("(%s: Line %d) ", __FILE__, __LINE__);   \
    printf(fmt, ##__VA_ARGS__);			    \
    } while (0) 
#else
#define DBUG(fmt, ...)
#endif // WITH_DEBUG

#define SLOS_ROOT_INODE (100000)
#define TOSMP(mp) ((struct slsfsmount *)(mp->mnt_data))
#define MPTOSLOS(mp) ((TOSMP(mp)->sp_slos))
#define SLSVP(vp) ((struct slos_node *)(vp->v_data))
#define SLSINO(svp) ((struct slos_inode *)(svp->sn_ino))
#define	VPSLOS(vp) (SLSVP(vp)->sn_slos)
#define SMPSLOS(smp) (mp->sdev->slos)

#define SVINUM(sp) (SLSINO(sp)->ino_pid)
#define VINUM(vp) (SVINUM(SLSVP(vp)))

#define SLS_VALLOC(aa, bb, cc, dd) ((TOSMP(aa->v_mount))->sls_valloc(aa, bb, cc, dd))
#define SLS_VNODE(mp, vp) getnewvnode("slsfs", mp, &sls_vnodeops, &vp)
#define SLS_VGET(aa, bb, cc, dd) (aa->v_mount->mnt_op->vfs_vget(aa->v_mount, bb, cc, dd))

struct slsfsmount {
    STAILQ_ENTRY(slsfsmount)	sp_next_mount;
    struct mount		*sp_vfs_mount;
    struct slsfs_device		*sp_sdev;
    struct slos			*sp_slos;
    int				sp_ronly;

    int (*sls_valloc)(struct vnode *, int, struct ucred *, struct vnode **);
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



extern struct vop_vector sls_vnodeops;
