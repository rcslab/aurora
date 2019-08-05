
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option detach_longopts[] = {
	{ "pid", required_argument, NULL, 'p' },
	{ NULL,	no_argument, NULL, 0},
};

void
detach_usage(void)
{
	printf("Usage: slsctl detach -p pid\n");
}

int
detach_main(int argc, char* argv[])
{
	int pid = -1;
	int opt;

	while ((opt = getopt_long(argc, argv, "p:", detach_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'p':
		pid = strtol(optarg, NULL, 10);
		break;
	    default:
		detach_usage();
		return 0;
	    }
	}

	if (optind != argc || pid == -1) {
	    detach_usage();
	    return 0;
	}

	if (sls_detach(pid) < 0)
	    return 1;

	return 0;
}
