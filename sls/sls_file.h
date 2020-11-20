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

int slsckpt_socket(struct proc *p, struct socket *so, 
	struct sbuf *sb, struct slsckpt_data *sckpt_data);
int slsrest_socket(struct slskv_table *table, struct slskv_table *sockbuftable, 
	struct slssock *info, struct slsfile *finfo, int *fdp);

int slsckpt_kqueue(struct proc *p, struct kqueue *kq, struct sbuf *sb);
int slsrest_kqueue(struct slskqueue *kqinfo, int *fdp);

int slsckpt_pipe(struct proc *p, struct file *fp, struct sbuf *sb);
int slsrest_pipe(struct slskv_table *table, int flags, struct slspipe *ppinfo, int *fdp);

int slsckpt_posixshm(struct shmfd *shmfd, struct sbuf *sb);
int slsrest_posixshm(struct slsposixshm *info, struct slskv_table *objtable, int *fdp);

int slsckpt_pts_mst(struct proc *p, struct tty *pts, struct sbuf *sb);
int slsckpt_pts_slv(struct proc *p, struct vnode *vp, struct sbuf *sb);
int slsrest_pts(struct slskv_table *filetable,  struct slspts *slspts, int *fdp);

int slsckpt_filedesc(struct proc *p, struct slsckpt_data *sckpt_data, struct sbuf *sb);
int slsrest_filedesc(struct proc *p, struct slsfiledesc *slsfiledesc, 
	struct slskv_table *fdtable, struct slsrest_data *restdata);

int slsrest_file(void *slsbacker, struct slsfile *info, struct slsrest_data *restdata);
int slsrest_knotes(int fd, slsset *slskns);

#endif /* _SLS_FD_H_ */