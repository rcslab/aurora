#ifndef _SLSMM_H_
#define _SLSMM_H_

#include <sys/types.h>

struct slsmm_param{
	int	fd;
	int	pid;
};

#define SLSMM_DUMP		_IOW('z', 1, struct slsmm_param)
#define SLSMM_RESTORE	_IOW('z', 2, struct slsmm_param)

#endif
