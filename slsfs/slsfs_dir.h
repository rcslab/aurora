#include <slos.h>
#include <slos_inode.h>
#include <slos_record.h>

struct componentname;

int slsfs_init_dir(struct slos_node *dvp, struct slos_node *vp, 
	struct componentname *);
uint64_t slsfs_add_dirent(struct slos_node *vp, uint64_t ino, 
	char *nameptr, long namelen, uint8_t type);
int slsfs_lookup_name(struct slos_node *dvp, 
	struct componentname *name, struct dirent *dir_p, struct slos_record **rec);
int slsfs_unlink_dir(struct slos_node *dvp, struct slos_node *vp, 
	struct componentname *name);

