#ifndef _SLOS_ALLOC_H_
#define _SLOS_ALLOC_H_

#include <sys/param.h>
#include <sys/sysctl.h>

#define OTREE(slos) (&((slos)->slos_alloc.a_offset->sn_tree))
#define STREE(slos) (&((slos)->slos_alloc.a_size->sn_tree))

int slos_allocator_init(struct slos *slos);
int uint64_t_comp(const void *k1, const void *k2);
int slos_allocator_uninit(struct slos *slos);
int slos_allocator_sync(struct slos *slos, struct slos_sb *newsb);
int slos_blkalloc(struct slos *slos, size_t bytes, diskptr_t *ptr);
int slos_blkalloc_large(struct slos *slos, size_t bytes, diskptr_t *ptr);
int slos_blkalloc_wal(struct slos *slos, size_t bytes, diskptr_t *ptr);

int slos_freebytes(SYSCTL_HANDLER_ARGS);

#endif
