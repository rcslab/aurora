#ifndef _SLS_FD_H_
#define _SLS_FD_H_

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/mman.h>
#include <sys/sbuf.h>

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_kv.h"

/* Maximum size of a UNIX socket address */
#define UNADDR_MAX (104)
#define DEVFS_ROOT "/dev/"

int slsckpt_socket(struct proc *p, struct socket *so, struct sbuf *sb,
    struct slsckpt_data *sckpt_data);
int slsrest_socket(struct slsrest_data *restdata, struct slssock *info,
    struct slsfile *finfo, int *fdp);

int slskq_checkpoint(
    struct proc *p, struct file *fp, struct slsfile *info, struct sbuf *sb);
int slskq_restore_kqueue(struct slskqueue *slskq, int *fdp);
int slskq_restore_knotes_all(struct proc *p, struct slskv_table *kevtable);

void slskq_attach_locked(struct proc *p, struct kqueue *kq);
void slskq_attach(struct proc *p, struct kqueue *kq);
void slskq_detach(struct kqueue *kq);

int slsckpt_pipe(struct proc *p, struct file *fp, struct sbuf *sb);
int slsrest_pipe(
    struct slskv_table *table, int flags, struct slspipe *ppinfo, int *fdp);

int slsckpt_posixshm(struct shmfd *shmfd, struct sbuf *sb);
int slsrest_posixshm(
    struct slsposixshm *info, struct slskv_table *objtable, int *fdp);

int slsckpt_pts_mst(struct proc *p, struct tty *pts, struct sbuf *sb);
int slsckpt_pts_slv(struct proc *p, struct vnode *vp, struct sbuf *sb);
int slsrest_pts(struct slskv_table *fptable, struct slspts *slspts, int *fdp);

int slsckpt_filedesc(
    struct proc *p, struct slsckpt_data *sckpt_data, struct sbuf *sb);
int slsrest_filedesc(struct proc *p, struct slsfiledesc *slsfiledesc,
    struct slsrest_data *restdata);

int slsrest_file(
    void *slsbacker, struct slsfile *info, struct slsrest_data *restdata);

#endif /* _SLS_FD_H_ */
