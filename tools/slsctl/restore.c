
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

static struct option restore_longopts[] = {
	{ "file", required_argument, NULL, 'f' },
	{ "memory", required_argument, NULL, 'm' },
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore <-f <filename> | -m <id>> \n");
}

int
restore_main(int argc, char* argv[]) {
	int error;
	int mode;
	int target;
	int opt;
	int target_set;
	pid_t pid;
	struct sls_backend backend;
	char *filename;

	pid = getpid();
	backend = (struct sls_backend) { 
		.bak_target = SLS_FILE,
		.bak_name = sbuf_new_auto(),
	};
	target_set = 0;

	while ((opt = getopt_long(argc, argv, "f:m:", restore_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'f':
		if (target_set == 1) {
		    restore_usage();
		    return 0;
		}

		backend.bak_target = SLS_FILE;
		error = sbuf_bcpy(backend.bak_name, optarg, PATH_MAX);
		if (error < 0) {
		    printf("sbuf_bcpy returned error %ld\n", error);
		    return 1;
		}

		sbuf_finish(backend.bak_name);
		break;

	    case 'm':
		if (target_set == 1) {
		    restore_usage();
		    return 0;
		}

		backend.bak_target = SLS_MEM;
		backend.bak_id = optarg;
		break;

	    /* XXX case 's' for the SLOS */
	    default:
		restore_usage();
		return 0;
	    }
	}

	if (optind != argc) {
	    restore_usage();
	    return 0;
	}

	if (sls_restore(pid, backend) < 0)
	    return 1;

	return 0;
}
