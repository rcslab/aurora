#ifndef __SLSMM_H__
#define __SLSMM_H__

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/ioccom.h>

#define SLSMM_DUMP _IOW('t', 1, int)

MALLOC_DECLARE(M_SLSMM);

#endif
