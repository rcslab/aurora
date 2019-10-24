#ifndef _SLS_FD_H_
#define _SLS_FD_H_

#include <sys/types.h>

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/sbuf.h>

#include "sls_data.h"

int slsckpt_filedesc(struct proc *p, struct sbuf *sb);
int slsrest_filedesc(struct proc *p, struct slsfiledesc info);

int slsckpt_file(struct proc *p, struct file *file, int fd, struct sbuf *sb);
int slsrest_file(struct proc *p, void *data, struct slsfile *file);

#endif /* _SLS_FD_H_ */
