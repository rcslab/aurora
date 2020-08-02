
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
	{ NULL,	no_argument, NULL, 0},
};

void
checkpoint_usage(void)
{
	printf("Usage: slsctl checkpoint <-p oid> [-r] \n");
}

int
checkpoint_main(int argc, char* argv[])
{
	int oid = -1;
	bool recurse = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "o:r", checkpoint_longopts, NULL)) != -1) {
		switch (opt) {
		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;
		case 'r':
			recurse = true;
			break;
		default:
			checkpoint_usage();
			return 0;
		}
	}

	if (optind != argc || oid == -1) {
		checkpoint_usage();
		return 0;
	}

	if (sls_checkpoint(oid, recurse) < 0)
		return 1;

	return 0;
}
