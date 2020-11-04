
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

static struct option checkpoint_longopts[] = {
	{ "memory", no_argument, NULL, 'm' },
	{ "oid", required_argument, NULL, 'o' },
	{ "recursive", no_argument, NULL, 'r' },
	{ "synchronous", no_argument, NULL, 's' },
	{ "help", no_argument, NULL, 'h' },
	{ NULL,	no_argument, NULL, 0},
};

void
checkpoint_usage(void)
{
	printf("Usage: slsctl checkpoint <-o oid> [-r] \n");
}

int
checkpoint_main(int argc, char* argv[])
{
	int opt;
	uint64_t oid = SLS_DEFAULT_PARTITION;
	bool recurse = false;
	bool synchronous = false;

	while ((opt = getopt_long(argc, argv, "mo:rsh", checkpoint_longopts, NULL)) != -1) {
		switch (opt) {
		case 'm':
			if (oid == SLS_DEFAULT_PARTITION) {
				oid = SLS_DEFAULT_MPARTITION;
			}
			break;
		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;
		case 'r':
			recurse = true;
			break;
		case 's':
			synchronous = true;
			break;
		case 'h':
		default:
			checkpoint_usage();
			return 0;
		}
	}

	if (optind != argc) {
		checkpoint_usage();
		return (0);
	}

	if (sls_checkpoint(oid, recurse, synchronous) < 0)
		return 1;

	return 0;
}
