
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
	{ "osd", required_argument, NULL, 'o' },
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore <-f <filename> | -o <ID>> \n");
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

	while ((opt = getopt_long(argc, argv, "f:m:o:", restore_longopts, NULL)) != -1) {
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
		target_set = 1;
		break;

	    case 'o':
		if (target_set == 1) {
		    restore_usage();
		    return 0;
		}

		/* Release the buffer allocated for the backend. */
		sbuf_finish(backend.bak_name);
		sbuf_delete(backend.bak_name);

		backend.bak_target = SLS_OSD;
		/* The id is the PID of the checkpointed process. */
		backend.bak_id = strtol(optarg, NULL, 10);
		
		target_set = 1;
		break;

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
