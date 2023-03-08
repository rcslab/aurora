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

static struct option partadd_memory_longopts[] = {
	{ "oid", required_argument, NULL, 'o' },
	{ "period", required_argument, NULL, 't' },
	{ "ignore unlinked files", required_argument, NULL, 'i' },
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_memory_usage()
{
	partadd_base_usage("memory", partadd_memory_longopts);
}

int
partadd_memory_main(int argc, char *argv[])
{
	struct sls_attr attr;
	uint64_t oid = 0;
	int port = 0;
	int error;
	int opt;

	attr = (struct sls_attr) {
		.attr_target = SLS_MEM,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = 0,
		.attr_amplification = 1,
	};

	while ((opt = getopt_long(argc, argv, "io:t:", partadd_memory_longopts,
		    NULL)) != -1) {
		switch (opt) {
		case 'i':
			attr.attr_flags |= SLSATTR_IGNUNLINKED;
			break;

		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;

		case 't':
			attr.attr_period = strtol(optarg, NULL, 10);
			break;

		default:
			printf("Invalid option '%c'\n", opt);
			partadd_memory_usage();
			return (0);
		}
	}

	if (oid == 0 || optind != argc) {
		partadd_memory_usage();
		return (0);
	}

	if (sls_partadd(oid, attr, -1) < 0)
		return (1);

	return (0);
}
