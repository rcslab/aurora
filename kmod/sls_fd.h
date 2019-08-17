#ifndef _SLS_FD_H_
#define _SLS_FD_H_

#include <sys/types.h>

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/sbuf.h>

#include "sls_data.h"

int sls_filedesc_ckpt(struct proc *p, struct sbuf *sb);
int sls_filedesc_rest(struct proc *p, struct filedesc_info info);

int sls_file_ckpt(struct proc *p, struct file *file, int fd, struct sbuf *sb);
int sls_file_rest(struct proc *p, void *data, struct file_info *file);

#endif /* _SLS_FD_H_ */
