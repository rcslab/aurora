#include <sys/mount.h>

#include "slos.h"

#define OTREE(slos) (&((slos)->slsfs_alloc.a_offset->sn_tree))
#define STREE(slos) (&((slos)->slsfs_alloc.a_size->sn_tree))

int slsfs_allocator_init(struct slos *slos);
int bootstrap_tree(struct slos *slos, size_t offset, diskptr_t *ptr);
int uint64_t_comp(const void *k1, const void *k2);
int slsfs_allocator_uninit(struct slos *slos);
int slsfs_allocator_sync(struct slos *slos, struct slos_sb *newsb);
