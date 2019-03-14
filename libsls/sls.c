
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sls.h"

int
sls_dump(struct dump_param *param)
{
	int status;
	int fd;
    
	fd = open("/dev/slsmm", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "ERROR: Could not open SLS device: %m");
		return -1;
	}

	status = ioctl(fd, SLSMM_DUMP, param);
	if (status < 0) {
		fprintf(stderr, "ERROR: SLS dump ioctl failed: %m");
	}

	close(fd);

	return status;
}

int
sls_restore(struct restore_param *param)
{
	int status;
	int fd;
    
	fd = open("/dev/slsmm", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "ERROR: Could not open SLS device: %m");
		return -1;
	}

	status = ioctl(fd, SLSMM_RESTORE, param);
	if (status < 0) {
		fprintf(stderr, "ERROR: SLS restore ioctl failed: %m");
	}

	close(fd);

	return status;
}
