#ifndef _SLSMM_H_
#define _SLSMM_H_

#include <sys/types.h>

#include <sys/ioccom.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/pcpu.h>
#include <sys/proc.h>
#include <sys/shm.h>

#include <machine/param.h>
#include <machine/pcb.h>
#include <machine/reg.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_map.h>

#include "hash.h"

struct dump_param {
	int	fd;
	int	pid;
};

struct restore_param {
	int pid;
	int nfds;
	int *fds;
};


#define FULL_DUMP		_IOW('d', 1, struct dump_param)
#define DELTA_DUMP		_IOW('d', 2, struct dump_param)
#define SLSMM_RESTORE	_IOW('r', 1, struct restore_param)

#define SLSMM_CKPT_FULL		0
#define SLSMM_CKPT_DELTA	1

#endif
