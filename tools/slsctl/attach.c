
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

static struct option attach_longopts[] = {
	{ "memory", no_argument, NULL, 'm' },
	{ "oid", required_argument, NULL, 'o' },
	{ "pid", required_argument, NULL, 'p' },
	{ NULL, no_argument, NULL, 0 },
};

void
attach_usage(void)
{
    printf("Usage: slsctl attach -p <pid> -o <id>\n");
}

int
attach_main(int argc, char* argv[]) {
	int pid_set, oid_set;
	uint64_t pid, oid;
	bool isrecursive = false;
	int error;
	int opt;

	pid = 0;
	oid = 0;
	pid_set = 0;
	oid_set = 0;

	while ((opt = getopt_long(argc, argv, "o:p:", attach_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'o':
		if (oid_set == 1) {
		    attach_usage();
		    return 0;
		}

		oid = strtol(optarg, NULL, 10);
		oid_set = 1;
		break;

	    case 'p':
		if (pid_set == 1) {
		    attach_usage();
		    return 0;
		}

		pid = strtol(optarg, NULL, 10);
		pid_set = 1;
		break;

	    default:
		attach_usage();
		return 0;
	    }
	}

	if (oid_set == 0 || pid_set == 0 || optind != argc) {
	    attach_usage();
	    return 0;
	}

	if (sls_attach(oid, pid) < 0)
	    return 1;

	return 0;

}
