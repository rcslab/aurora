#include <sys/types.h>

#include <sys/ioctl.h>
#include <sys/sbuf.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option restore_longopts[] = {
	{ "daemon", no_argument, NULL, 'd' },
	{ "rest_stopped", no_argument, NULL, 's' },
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore -o <oid> [-d] [-s]\n");
}

int
restore_main(int argc, char* argv[]) {
	int error;
	int status;
	int oid_set;
	uint64_t oid;
	pid_t childpid;
	bool daemon = false;
	bool rest_stopped = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "do:s", restore_longopts, NULL)) != -1) {
		switch(opt) {
		case 'd':
			/*
			 * The proceses are restored as a daemon, detached from
			 * the restore process' terminal.
			 */
			daemon = true;
			break;

		case 'o':
			if (oid_set == 1) {
				restore_usage();
				return (0);
			}
			/* The id is the PID of the checkpointed process. */
			oid = strtol(optarg, NULL, 10);

			oid_set = 1;
			break;

		case 's':
			/*
			 * The proceses are restored as a daemon, detached from
			 * the restore process' terminal.
			 */
			rest_stopped = true;
			break;

		default:
			restore_usage();
			return (0);
		}
	}

	if (optind != argc) {
		restore_usage();
		return (0);
	}

	if ((error = sls_restore(oid, daemon, rest_stopped)) < 0)
		return (1);

	/*
	 * Wait for all children. If we end up
	 * with no children the program exits.
	 */
	for (;;) {
		childpid = wait(&status);
		if (childpid < 0) {
			perror("wait");
			return (0);
		}
	}

	return (0);
}
