
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
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_usage(void)
{
    printf("Usage: slsctl partadd -o <id> [-t ms] [--delta]\n");
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

	/* XXX Allow -m option back when we are ready for in-memory checkpointing. */
	while ((opt = getopt_long(argc, argv, "do:p:t:", partadd_longopts, NULL)) != -1) {
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
