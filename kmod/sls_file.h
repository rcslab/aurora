#ifndef _SLS_FD_H_
#define _SLS_FD_H_

#include <sys/types.h>

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/sbuf.h>

#include "sls_data.h"
#include "sls_internal.h"
#include "sls_kv.h"

/* Maximum size of a UNIX socket address */
#define UNADDR_MAX (104)

int slsckpt_socket(struct proc *p, struct socket *so, struct sbuf *sb);
int slsrest_socket(struct slskv_table *table, struct slssock *info, struct slsfile *finfo, int *fdp);

int slsckpt_kqueue(struct proc *p, struct kqueue *kq, struct sbuf *sb);
int slsrest_kqueue(struct slskqueue *kqinfo, int *fdp);

int slsckpt_pipe(struct proc *p, struct file *fp, struct sbuf *sb);
int slsrest_pipe(struct slskv_table *table, struct slspipe *ppinfo, int *fdp);

int slsckpt_filedesc(struct proc *p, struct slskv_table *objtable, struct sbuf *sb);
int slsrest_filedesc(struct proc *p, struct slsfiledesc info, 
	struct slskv_table *fdtable, struct slskv_table *filetable);

int slsrest_file(void *slsbacker, struct slsfile *info, struct slsrest_data *restdata);
int slsrest_kevents(int fd, slsset *slskevs);

#endif /* _SLS_FD_H_ */
