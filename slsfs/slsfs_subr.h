#include <sys/types.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/buf.h>

#include <slos.h>
#include <slos_inode.h>

int slsfs_uninit(struct vfsconf *vfsp);
int slsfs_init(struct vfsconf *vfsp);
int slsfs_getnode(struct slos *, uint64_t ino, struct slos_node **spp);
int slsfs_new_node(struct slos *, mode_t mode, uint64_t *pid);
int slsfs_remove_node(struct vnode *dvp, struct vnode *vp, struct componentname *name);
int slsfs_bufwrite(struct buf *buf);
int slsfs_bufsync(struct bufobj *bufobj, int waitfor);
void slsfs_bdflush(struct bufobj *bufobj, struct buf *buf);
int slsfs_sync_vp(struct vnode *vp);
void slsfs_bufstrategy(struct bufobj *bo, struct buf *bp);
