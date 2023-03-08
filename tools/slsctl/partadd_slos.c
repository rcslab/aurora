#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "partadd.h"

static struct option partadd_slos_longopts[] = {
	{ "amplification", required_argument, NULL, 'a' },
	{ "cached restore", required_argument, NULL, 'c' },
	{ "delta", no_argument, NULL, 'd' },
	{ "precopy", required_argument, NULL, 'e' },
	{ "ignore unlinked files", required_argument, NULL, 'i' },
	{ "lazy restore", required_argument, NULL, 'l' },
	{ "oid", required_argument, NULL, 'o' },
	{ "prefault", required_argument, NULL, 'p' },
	{ "period", required_argument, NULL, 't' },
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_slos_usage(void)
{
	partadd_base_usage("slos", partadd_slos_longopts);
}

int
partadd_slos_main(int argc, char *argv[])
{
	struct sls_attr attr;
	uint64_t oid = 0;
	int error;
	int opt;
	int ret;

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = 0,
		.attr_amplification = 1,
	};

	while ((opt = getopt_long(argc, argv,
		    "a:cdeilo:pt:", partadd_slos_longopts, NULL)) != -1) {
		switch (opt) {
		case 'a':
			/* Checkpoint amplification factor. */
			attr.attr_amplification = strtol(optarg, NULL, 10);
			break;
		case 'c':
			/* Prefaults only make sense for lazy restores. */
			attr.attr_flags |= (SLSATTR_CACHEREST |
			    SLSATTR_LAZYREST);
			break;

		case 'd':
			attr.attr_mode = SLS_DELTA;
			break;

		case 'e':
			/*
			 * Due to the way we mark the hot set, we need to
			 * prefault to be able to precopy.
			 */
			attr.attr_flags |= (SLSATTR_LAZYREST |
			    SLSATTR_PREFAULT | SLSATTR_PRECOPY);
			break;

		case 'i':
			attr.attr_flags |= SLSATTR_IGNUNLINKED;
			break;

		case 'l':
			attr.attr_flags |= SLSATTR_LAZYREST;
			break;

		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;

		case 'p':
			attr.attr_flags |= SLSATTR_PREFAULT;
			break;

		case 't':
			attr.attr_period = strtol(optarg, NULL, 10);
			break;

		default:
			printf("Invalid option '%c'\n", opt);
			partadd_slos_usage();
			return (0);
		}
	}

	if (oid == 0 || optind != argc) {
		partadd_slos_usage();
		return (0);
	}

	if (sls_partadd(oid, attr, -1) < 0)
		return (1);

	return (0);
}
