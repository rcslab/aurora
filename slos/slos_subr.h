#ifndef _SLOS_SUBR_H_
#define _SLOS_SUBR_H_

int slos_get_node(struct slos *, uint64_t ino, struct slos_node **spp);
int slos_remove_node(struct vnode *dvp, struct vnode *vp, struct componentname *name);
int slos_destroy_node(struct slos_node *vp);

int slos_truncate(struct vnode *vp, size_t size);
int slos_setupfakedev(struct slos *slos, struct slos_node *vp);
void slos_generic_rc(void *ctx, bnode_ptr p);

int slos_bufwrite(struct buf *buf);
int slos_bufsync(struct bufobj *bufobj, int waitfor);
void slos_bufbdflush(struct bufobj *bufobj, struct buf *buf);
void slos_bufstrategy(struct bufobj *bo, struct buf *bp);

int slos_sync_vp(struct vnode *vp, int release);

#endif
