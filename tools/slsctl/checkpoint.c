
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
	{ "oid", required_argument, NULL, 'o' },
	{ "recursive", no_argument, NULL, 'r' },
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

	while ((opt = getopt_long(argc, argv, "o:rh", checkpoint_longopts, NULL)) != -1) {
		switch (opt) {
		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;
		case 'r':
			recurse = true;
			break;
		case 'h':
		default:
			checkpoint_usage();
			return 0;
		}
	}

	if (optind != argc) {
		checkpoint_usage();
		return 0;
	}

	if (sls_checkpoint(oid, recurse) < 0)
		return 1;

	return 0;
}
