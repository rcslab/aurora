
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

static struct option attach_longopts[] = {
	{ "delta", no_argument, NULL, 'd' },
	{ "file", required_argument, NULL, 'f' },
	{ "memory", no_argument, NULL, 'm' },
	{ "osd", required_argument, NULL, 'o' },
	{ "pid", required_argument, NULL, 'p' },
	{ "period", required_argument, NULL, 't' },
	{ NULL, no_argument, NULL, 0 },
};

void
attach_usage(void)
{
    printf("Usage: slsctl attach [-p <PID>] <-f <filename> | -m | -o <id>> [-t ms] [--delta]\n");
}

int
attach_main(int argc, char* argv[]) {
	pid_t pid;
	int error;
	int mode;
	int target;
	struct sls_attr attr;
	int target_set;
	int pid_set;
	int opt;
	char *filename;

	pid = 0;
	attr = (struct sls_attr) { 
		.attr_backend = (struct sls_backend) {
		    .bak_target = SLS_FILE,
		    .bak_name = sbuf_new_auto(),
		},
		.attr_mode = SLS_FULL,
		.attr_period = 0,
	};
	pid_set = 0;
	target_set = 0;

	while ((opt = getopt_long(argc, argv, "adf:mop:t:", attach_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'd':
		attr.attr_mode = SLS_DELTA;
		break;
	    case 'f':
		if (target_set == 1) {
		    attach_usage();
		    return 0;
		}

		attr.attr_backend.bak_target = SLS_FILE;
		error = sbuf_bcpy(attr.attr_backend.bak_name, optarg, PATH_MAX);
		if (error < 0) {
		    printf("sbuf_bcpy returned error %d\n", error);
		    return 1;
		}

		truncate(optarg, 0);
		target_set = 1;
		break;
	    case 'm':
		if (target_set == 1) {
		    attach_usage();
		    return 0;
		}

		attr.attr_backend.bak_target = SLS_MEM;
		target_set = 1;
		break;

	    case 'o':
		/* XXX Require a PID argument */
		attr.attr_backend.bak_target = SLS_OSD;
		target_set = 1;
		break;

	    case 'p':
		pid_set = 1;
		pid = strtol(optarg, NULL, 10);
		break;

	    case 't':
		attr.attr_period = strtol(optarg, NULL, 10);
		break;

	    default:
		attach_usage();
		return 0;
	    }
	}

	if (target_set == 0 || pid_set == 0 || optind != (argc - 1)) {
	    attach_usage();

	    return 0;
	}

	if (sls_attach(pid, attr) < 0)
	    return 1;

	return 0;

}
