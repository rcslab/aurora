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
#include <errno.h>

#include <sls.h>

static struct option restore_longopts[] = {
	{ "daemon", no_argument, NULL, 'd' },
	{ "memory", no_argument, NULL, 'm' },
	{ "rest_stopped", no_argument, NULL, 's' },
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore -o <oid> [-m] [-d] [-s]\n");
}

int
restore_main(int argc, char* argv[]) {
	int error;
	int status;
	uint64_t oid = SLS_DEFAULT_PARTITION;
	pid_t childpid;
	bool daemon = false;
	bool rest_stopped = false;
	int opt;

	while ((opt = getopt_long(argc, argv, "dmo:s", restore_longopts, NULL)) != -1) {
		switch(opt) {
		case 'd':
			/*
			 * The proceses are restored as a daemon, detached from
			 * the restore process' terminal.
			 */
			daemon = true;
			break;
		case 'm':
			if (oid == SLS_DEFAULT_PARTITION)
			    oid = SLS_DEFAULT_MPARTITION;
			break;

		case 'o':
			/* The id is the PID of the checkpointed process. */
			oid = strtol(optarg, NULL, 10);
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
	while (1) {
		childpid = wait(&status);
		if (childpid < 0 && errno == ECHILD) {
			/* No more children to wait for. */
			printf("No more children\n");
			return (0);
		} else if (childpid < 0) {
			perror("wait");
			return (-1);
		} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			return (status);
		} else if (WIFSIGNALED(status)) {
			if (WTERMSIG(status) == SIGSEGV) {
				fprintf(stderr, "Process segfaulted\n");
				return (status);
			}
			return (status);
		}
	}

	return (0);
}
