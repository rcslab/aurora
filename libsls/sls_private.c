
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sls_private.h"

int
sls_op(struct op_param *param)
{
	int status;
	int fd;
    
	fd = open("/dev/sls", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "ERROR: Could not open SLS device: %m");
		return -1;
	}

	status = ioctl(fd, SLS_OP, param);
	if (status < 0) {
		fprintf(stderr, "ERROR: SLS op ioctl failed: %m");
	}

	close(fd);

	return status;
}

int
sls_proc(struct proc_param *param)
{
	int status;
	int fd;

	fd = open("/dev/sls", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "ERROR: Could not open SLS device: %m");
		return -1;
	}

	status = ioctl(fd, SLS_PROC, param);
	if (status < 0) {
		fprintf(stderr, "ERROR: SLS proc ioctl failed: %m");
	}

	close(fd);

	return status;

}

