
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>

#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct option partdel_longopts[] = {
	{ "oid", required_argument, NULL, 'o' },
	{ NULL, no_argument, NULL, 0 },
};

void
partdel_usage(void)
{
	printf("Usage: slsctl partdel -o oid\n");
}

int
partdel_main(int argc, char *argv[])
{
	int oid = -1;
	int opt;

	while ((opt = getopt_long(argc, argv, "o:", partdel_longopts, NULL)) !=
	    -1) {
		switch (opt) {
		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;
		default:
			partdel_usage();
			return 0;
		}
	}

	if (optind != argc || oid == -1) {
		partdel_usage();
		return 0;
	}

	if (sls_partdel(oid) < 0)
		return 1;

	return 0;
}
