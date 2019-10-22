#include <sys/types.h>
#include <sys/systm.h>
#include <sys/unistd.h>

#include "slos_internal.h"
#include "slos_inode.h"

int slsfs_uninit(struct vfsconf *vfsp);
int slsfs_init(struct vfsconf *vfsp);
int slsfs_getnode(struct slos *, uint64_t ino, struct slos_node **);
int slsfs_newnode(struct slos *, struct slos_node **);
