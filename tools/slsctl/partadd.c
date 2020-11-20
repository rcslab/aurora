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
	{ "ignore unlinked files", required_argument, NULL, 'i' },
	{ "lazy restore", required_argument, NULL, 'l' },
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_usage(void)
{
	struct option opt;
	int i;

	printf("Usage: slsctl partadd -o <id>  [-b <memory|slos>] [-t ms] [options]\n");
	printf("Full options list:\n");
	i = 0;
	for (opt = partadd_longopts[0]; opt.name != NULL; opt = partadd_longopts[++i])
		printf("-%c\t%s\n", opt.val, opt.name);

	exit(0);
}

int
partadd_main(int argc, char* argv[])
{
	int opt;
	int error;
	struct sls_attr attr;
	uint64_t oid = 0;
	int mode;

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		    .attr_mode = SLS_FULL,
		    .attr_period = 0,
		    .attr_flags = 0,
	};

	while ((opt = getopt_long(argc, argv, "b:dilo:p:t:", partadd_longopts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			attr.attr_mode = SLS_DELTA;
			break;

		case 'i':
			attr.attr_flags |= SLSATTR_IGNUNLINKED;
			break;

		case 'l':
			attr.attr_flags |= SLSATTR_LAZYREST;
			break;

		case 'm':
			attr.attr_target = SLS_MEM;
			break;

		case 'o':
			oid = strtol(optarg, NULL, 10);
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

	if (oid == 0 || optind != argc) {
		partadd_usage();
		return 0;
	}

	if (sls_partadd(oid, attr) < 0)
		return 1;

	return 0;

}
