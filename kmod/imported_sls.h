#ifndef _COPIED_FD_H
#define _COPIED_FD_H

#include <sys/param.h>
#include <sys/fnv_hash.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

/* Copied from kern_descrip.c */

#define NDFILE		20
#define NDSLOTSIZE	sizeof(NDSLOTTYPE)
#define	NDENTRIES	(NDSLOTSIZE * __CHAR_BIT)
#define NDSLOT(x)	((x) / NDENTRIES)
#define NDBIT(x)	((NDSLOTTYPE)1 << ((x) % NDENTRIES))
#define	NDSLOTS(x)	(((x) + NDENTRIES - 1) / NDENTRIES)


int fd_first_free(struct filedesc *fdp, int low, int size);
int fdisused(struct filedesc *fdp, int fd);
void fdused_init(struct filedesc *fdp, int fd);
void fdused(struct filedesc *fdp, int fd);

int kern_chroot(struct thread *td, char *path, enum uio_seg segflg);

int dofileread(struct thread *td, int fd, struct file *fp, struct uio *auio,
    off_t offset, int flags);
int dofilewrite(struct thread *td, int fd, struct file *fp, struct uio *auio,
    off_t offset, int flags);

void bwillwrite(void);

int kqueue_acquire(struct file *fp, struct kqueue **kqp);
void kqueue_release(struct kqueue *kq, int locked);


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


#endif /* _COPIED_FD_H */

