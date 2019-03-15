#ifndef _SLSMM_H_
#define _SLSMM_H_

#include <sys/types.h>

#include <sys/ioccom.h>
#include <sys/mman.h>
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

/* XXX Merge dump and restore */
struct dump_param {
	void	*name;
	size_t	len;
	int	pid;
	int	fd_type;
	int	dump_mode;
	int	async;
};

struct restore_param {
	void	*name;
	size_t	len;
	int	pid;
	int	fd_type;
};

struct compose_param {
	int	nfds;
	int	*fds;
	int	fd_type;
	int	outfd;
	int	outfd_type;
};


#define SLSMM_DUMP		_IOWR('d', 1, struct dump_param)
#define SLSMM_FLUSH_COUNT	_IOR('d', 3, int)
#define SLSMM_RESTORE		_IOW('d', 4, struct restore_param)
#define SLSMM_COMPOSE		_IOWR('d', 5, struct compose_param)

#define SLSMM_CKPT_FULL		0
#define SLSMM_CKPT_DELTA	1

#define SLSMM_CKPT_SYNC		0
#define SLSMM_CKPT_ASYNC	1

#define SLSMM_FD_INVALID_LOW 	0
#define SLSMM_FD_FILE	 	1
#define SLSMM_FD_MEM	 	2
#define SLSMM_FD_NVDIMM	 	3
#define SLSMM_FD_FD	 	4
#define SLSMM_FD_INVALID_HIGH 	5

#endif
