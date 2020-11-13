#ifndef _COPIED_FD_H
#define _COPIED_FD_H

#include <sys/param.h>
#include <sys/fnv_hash.h>
#include <sys/mman.h>
#include <sys/tty.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

extern int shm_last_free, shm_nused, shmalloced;
extern vm_size_t shm_committed;
extern struct shmid_kernel *shmsegs;

struct shmmap_state {
    vm_offset_t va;
    int shmid;
};

#define	SHMSEG_FREE     	0x0200
#define	SHMSEG_REMOVED  	0x0400
#define	SHMSEG_ALLOCATED	0x0800

MALLOC_DECLARE(M_SHM);
MALLOC_DECLARE(M_SHMFD);

struct shm_mapping {
    char		*sm_path;
    Fnv32_t		sm_fnv;
    struct shmfd	*sm_shmfd;
    LIST_ENTRY(shm_mapping) sm_link;
};

extern u_long shm_hash;
extern LIST_HEAD(, shm_mapping) *shm_dictionary;

struct ttyoutq_block {
    struct ttyoutq_block	*tob_next;
    char			tob_data[TTYOUTQ_DATASIZE];
};

#define BMSIZE			32
#define TTYINQ_QUOTESIZE	(TTYINQ_DATASIZE / BMSIZE)

struct ttyinq_block {
    struct ttyinq_block	*tib_prev;
    struct ttyinq_block	*tib_next;
    uint32_t		tib_quotes[TTYINQ_QUOTESIZE];
    char			tib_data[TTYINQ_DATASIZE];
};


#endif /* _COPIED_FD_H */

