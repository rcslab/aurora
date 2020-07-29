#ifndef _SLSFS_H_
#define _SLSFS_H_
#include <sys/limits.h>
#include <sys/param.h>
#include <sys/queue.h>

#include <sys/mount.h>
#include <sys/vnode.h>
#include <vm/uma.h>

#include "slos.h"

struct fbtree;

#define SLOS_INODES_ROOT (0)
#define SLOS_ROOT_INODE (1)
#define SLOS_SYSTEM_MAX (2)

#define EPOCH_INVAL (UINT64_MAX)

#define TOSMP(mp) ((struct slsfsmount *)(mp->mnt_data))
#define MPTOSLOS(mp) ((TOSMP(mp)->sp_slos))
#define SLSVP(vp) ((struct slos_node *)(vp->v_data))
#define SLSVPSIZ(vp) (SLSINO(vp).ino_size)
#define SLSINO(svp) ((svp)->sn_ino)
#define	VPSLOS(vp) (SLSVP(vp)->sn_slos)
#define SMPSLOS(mp) ((mp)->sdev->slos)
#define SECTORSIZE(smp) ((smp)->sp_sdev->devblocksize)
#define IOSIZE(svp) (BLKSIZE((svp)->sn_slos))
#define BLKSIZE(slos) ((slos)->slos_sb->sb_bsize)

#define SVINUM(sp) (SLSINO(sp).ino_pid)
#define VINUM(vp) (SVINUM(SLSVP(vp)))

#define SLS_VALLOC(aa, bb, cc, dd) ((TOSMP(aa->v_mount))->sls_valloc(aa, bb, cc, dd))
#define SLS_VNODE(mp, vp) getnewvnode("slsfs", mp, &sls_vnodeops, &vp)
#define SLS_VGET(aa, bb, cc, dd) (aa->v_mount->mnt_op->vfs_vget(aa->v_mount, bb, cc, dd))

extern uma_zone_t fnode_zone;
extern struct buf_ops bufops_slsfs;

struct slsfs_getsnapinfo {
	int index;
	struct slos_sb snap_sb;
};

#define SLSFS_SNAP (0x1)

#define SLSFS_GET_SNAP		_IOWR('N', 100, struct slsfs_getsnapinfo)
#define SLSFS_MOUNT_SNAP	_IOWR('N', 101, struct slsfs_getsnapinfo)

struct slsfsmount {
    STAILQ_ENTRY(slsfsmount)	sp_next_mount;
    struct mount		*sp_vfs_mount;
    struct slsfs_device		*sp_sdev;
    struct slos			*sp_slos;
    int				sp_ronly;
    int				sp_index;

    int (*sls_valloc)(struct vnode *, mode_t, struct ucred *, struct vnode **);
};

struct slsfs_device {
    struct vnode		*devvp;
    void 			*vdata;
    struct mtx			g_mtx; 
    struct g_provider		*gprovider;
    struct g_consumer		*gconsumer;

    uint64_t			refcnt;
    uint64_t			devsize;
    uint64_t			devblocksize;
};

/* Needed by the SLS to create nodes with specific IDs. */
int slsfs_new_node(struct slos *slos, mode_t mode, uint64_t *oidp);
int slsfs_fbtree_rangeinsert(struct fbtree *tree, uint64_t lbn, uint64_t size);
int vmobjecttest(struct slos *slos);
int slsfs_cksum(struct buf *bp);

#define SLS_SEEK_EXTENT _IOWR('s', 1, struct uio *)
#define SLS_SET_RSTAT	_IOWR('s', 2, struct slos_rstat *)
#define SLS_GET_RSTAT	_IOWR('s', 3, struct slos_rstat *)

extern int checksum_enabled;

extern struct vop_vector sls_vnodeops;
extern struct vop_vector sls_fifoops;
#endif // _SLSFS_H_
