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

static struct option epoch_longopts[] = {
	{ "oid", required_argument, NULL, 'o' },
	{ NULL,	no_argument, NULL, 0},
};

void
epoch_usage(void)
{
	printf("Usage: slsctl epoch -o oid\n");
}

int
epoch_main(int argc, char* argv[])
{
	uint64_t oid = -1;
	uint64_t epoch;	
	int opt;

	while ((opt = getopt_long(argc, argv, "o:", epoch_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'o':
		oid = strtol(optarg, NULL, 10);
		break;
	    default:
		epoch_usage();
		return 0;
	    }
	}

	if (optind != argc || oid == -1) {
	    epoch_usage();
	    return 0;
	}

	if (sls_epoch(oid, &epoch) < 0)
		return -1;

	printf("%ld\n", epoch);
	return 0;
}
