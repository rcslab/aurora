#include <sys/types.h>
#include <sys/systm.h>
#include <sys/unistd.h>
#include <sys/buf.h>

#include <slos.h>
#include <slos_inode.h>

int slsfs_get_node(struct slos *, uint64_t ino, struct slos_node **spp);
int slsfs_remove_node(struct vnode *dvp, struct vnode *vp, struct componentname *name);
int slsfs_destroy_node(struct slos_node *vp);

int slsfs_truncate(struct vnode *vp, size_t size);

int slsfs_bufwrite(struct buf *buf);
int slsfs_bufsync(struct bufobj *bufobj, int waitfor);
void slsfs_bufbdflush(struct bufobj *bufobj, struct buf *buf);
void slsfs_bufstrategy(struct bufobj *bo, struct buf *bp);

int slsfs_sync_vp(struct vnode *vp);
int slsfs_sync_dev(struct vnode *vp);

#define EOF (-1)

SDT_PROVIDER_DECLARE(slos);
