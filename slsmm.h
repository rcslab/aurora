#include <sys/ioccom.h>

#define SLSMM_READ  _IOR('t', 1, char)
#define SLSMM_WRITE _IOW('t', 2, char)
