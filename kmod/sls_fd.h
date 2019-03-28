#ifndef _FD_H_
#define _FD_H_

#include <sys/types.h>

#include <sys/file.h>
#include <sys/filedesc.h>

#include "sls_data.h"

int fd_checkpoint(struct proc *p, struct filedesc_info *filedesc_info);
int fd_restore(struct proc *p, struct filedesc_info *info);

#endif