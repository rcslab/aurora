
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
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore -o <oid> \n");
}

int
restore_main(int argc, char* argv[]) {
	int error;
	int oid_set;
	uint64_t oid;
	int opt;	

	while ((opt = getopt_long(argc, argv, "o:", restore_longopts, NULL)) != -1) {
	    switch(opt) {
	    case 'o':
		if (oid_set == 1) {
		    restore_usage();
		    return 0;
		}
		/* The id is the PID of the checkpointed process. */
		oid = strtol(optarg, NULL, 10);
		
		oid_set = 1;
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

	if (sls_restore(oid) < 0)
	    return 1;

	return 0;
}
