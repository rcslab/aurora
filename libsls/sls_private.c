#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sls_private.h"

static int _slsfd = -1;

/* Generic ioctl for the SLS device. */
int
sls_ioctl(long ionum, void *args)
{
	int status;

	if (_slsfd < 0) {
		_slsfd = open("/dev/sls", O_RDWR);
		if (_slsfd < 0) {
			fprintf(
			    stderr, "ERROR: Could not open SLS device: %m\n");
			return (-1);
		}
	}

	status = ioctl(_slsfd, ionum, args);
	if (status < 0) {
		fprintf(stderr, "ERROR: SLS proc ioctl failed: %m\n");
		return (-1);
	}

	// close(fd);

	return (status);
}
