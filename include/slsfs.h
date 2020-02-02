#include <sys/mount.h>
#include <sys/queue.h>

struct slos;


#define SLOS_ROOT_INODE (100000)
#define TOSMP(mp) ((struct slsfsmount *)(mp->mnt_data))
#define MPTOSLOS(mp) ((TOSMP(mp)->sp_slos))
#define SLSVP(vp) ((struct slos_node *)(vp->v_data))
#define SLSVPSIZ(vp) (SLSINO(vp)->ino_size)
#define SLSINO(svp) ((svp)->sn_ino)
#define	VPSLOS(vp) (SLSVP(vp)->sn_slos)
#define SMPSLOS(mp) ((mp)->sdev->slos)
#define SECTORSIZE(smp) ((smp)->sp_sdev->devblocksize)
#define IOSIZE(svp) ((svp)->sn_slos->slos_sb->sb_bsize)

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

    int (*sls_valloc)(struct vnode *, mode_t, struct ucred *, struct vnode **);
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
