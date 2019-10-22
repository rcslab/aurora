#include "slos_inode.h"
#include "slos_internal.h"

int slsfs_create_dir(struct slos *slos, uint64_t ino, uint64_t parent, struct slos_node **vpp);
int slsfs_add_dirent(struct slos_node *vp, uint64_t ino, char *nameptr, long namelen, uint8_t type);
