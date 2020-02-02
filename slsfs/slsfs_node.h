#include <sys/types.h>

#include <slos.h>
#include <slos_inode.h>
#include <slos_record.h>

int slsfs_key_remove(struct slos_node *svp, uint64_t key);
int slsfs_key_insert(struct slos_node *svp, uint64_t key, struct slos_recentry val);

