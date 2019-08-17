#ifndef _COPIED_FD_H
#define _COPIED_FD_H

/* Copied form kern_descrip.c */

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

#endif /* _COPIED_FD_H */

