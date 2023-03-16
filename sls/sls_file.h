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

int slspts_checkpoint_vnode(struct vnode *vp, struct sbuf *sb);

int slskq_restore_knotes_all(struct proc *p, struct slskv_table *kevtable);

void slskq_attach_locked(struct proc *p, struct kqueue *kq);
void slskq_attach(struct proc *p, struct kqueue *kq);
void slskq_detach(struct kqueue *kq);

int slsckpt_filedesc(
    struct proc *p, struct slsckpt_data *sckpt_data, struct sbuf *sb);
int slsrest_filedesc(struct proc *p, struct slsfiledesc *slsfiledesc,
    struct slsrest_data *restdata);

int slsrest_file(
    void *slsbacker, struct slsfile *info, struct slsrest_data *restdata);
bool slsckpt_vnode_istty(struct vnode *vp);

int slsfile_extractfp(int fd, struct file **fpp);
void slsfile_attemptclose(int fd);

struct slsfile_ops {
	bool (*slsfile_supported)(struct file *);
	int (*slsfile_slsid)(struct file *, uint64_t *);
	int (*slsfile_checkpoint)(
	    struct file *fp, struct sbuf *sb, struct slsckpt_data *sckpt_data);
	int (*slsfile_restore)(void *slsbacker, struct slsfile *finfo,
	    struct slsrest_data *restdata, struct file **fpp);
};

#endif /* _SLS_FD_H_ */
