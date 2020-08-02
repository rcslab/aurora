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

static struct option partadd_longopts[] = {
	{ "delta", no_argument, NULL, 'd' },
	{ "memory", no_argument, NULL, 'm' },
	{ "oid", required_argument, NULL, 'o' },
	{ "period", required_argument, NULL, 't' },
	{ "backend", required_argument, NULL, 'b' },
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_usage(void)
{
	printf("Usage: slsctl partadd -o <id>  [-b <memory|slos>] [-t ms] [-d]\n");
}

int
partadd_main(int argc, char* argv[]) {
	struct sls_attr attr;
	uint64_t oid;
	int oid_set;
	int error;
	int mode;
	int opt;

	attr = (struct sls_attr) { 
		.attr_target = SLS_OSD,
		    .attr_mode = SLS_FULL,
		    .attr_period = 0,
	};
	oid_set = 0;

	while ((opt = getopt_long(argc, argv, "b:do:p:t:", partadd_longopts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			attr.attr_mode = SLS_DELTA;
			break;

		case 'm':
			attr.attr_target = SLS_MEM;
			break;

		case 'o':
			oid = strtol(optarg, NULL, 10);
			oid_set = 1;
			break;

		case 't':
			attr.attr_period = strtol(optarg, NULL, 10);
			break;

		case 'b':

			/* The input is only valid if we matched a string. */
			if (strncmp(optarg, "memory", sizeof("memory")) == 0)
				attr.attr_target = SLS_MEM;
			else if (strncmp(optarg, "slos", sizeof("slos")) == 0)
				attr.attr_target = SLS_OSD;
			else
				partadd_usage();

			break;

		default:
			partadd_usage();
			return 0;
		}
	}

	if (oid_set == 0 || optind != argc) {
		partadd_usage();
		return 0;
	}

	if (sls_partadd(oid, attr) < 0)
		return 1;

	return 0;

}
