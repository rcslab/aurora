
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
	{ "help", no_argument, NULL, 'h' },
	{ NULL, no_argument, NULL, 0 },
};

void
attach_usage(void)
{
	printf("Usage: slsctl attach -p <pid> -o <id> -m\n");
}

int
attach_main(int argc, char* argv[])
{
	int opt;
	int error;
	uint64_t pid = 0;
	uint64_t oid = 0;
	bool isrecursive = false;

	pid = 0;
	oid = 0;

	while ((opt = getopt_long(argc, argv, "o:p:mh", attach_longopts, NULL)) != -1) {
		switch (opt) {
		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;

		case 'm':
			if (oid == 0) {
				oid = SLS_DEFAULT_MPARTITION;
			}
			break;

		case 'p':
			pid = strtol(optarg, NULL, 10);
			break;

		case 'h':
		default:
			attach_usage();
			return 0;
		}
	}

	if (oid == 0) {
		oid = SLS_DEFAULT_PARTITION;
	}

	if (pid == 0 || optind != argc) {
		attach_usage();
		return 0;
	}

	if (sls_attach(oid, pid) < 0)
		return 1;

	return 0;

}
