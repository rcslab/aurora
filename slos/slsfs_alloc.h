#include <sys/mount.h>

#include "slos.h"

#define OTREE(slos) (&((slos)->slsfs_alloc.a_offset->sn_tree))
#define STREE(slos) (&((slos)->slsfs_alloc.a_size->sn_tree))

int slsfs_allocator_init(struct slos *slos);
int slsfs_allocator_sync(struct slos *slos);
