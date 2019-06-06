#ifndef _FD_H_
#define _FD_H_

#include <sys/types.h>

#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/sbuf.h>

#include "sls_data.h"

int fd_ckpt(struct proc *p, struct sbuf *sb);
int fd_rest(struct proc *p, struct filedesc_info info);

#endif
