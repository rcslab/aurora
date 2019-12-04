#include <sys/types.h>
#include <sys/systm.h>
#include <sys/unistd.h>

#include <slos.h>
#include <slos_inode.h>

int slsfs_uninit(struct vfsconf *vfsp);
int slsfs_init(struct vfsconf *vfsp);
int slsfs_getnode(struct slos *, uint64_t ino, struct slos_node **spp);
int slsfs_new_node(struct slos *, int mode, uint64_t *pid);
int slsfs_remove_node(struct slos_node *dvp, struct slos_node *vp, struct componentname *name);
