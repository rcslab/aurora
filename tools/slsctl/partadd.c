#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "partadd.h"

struct partcmd {
	const char *name;
	void (*usage)();
	int (*cmd)(int, char **);
} partcmdtable[] = { { "slos", &partadd_slos_usage, &partadd_slos_main },
	{ "memory", &partadd_memory_usage, &partadd_memory_main },
	{ "file", &partadd_file_usage, &partadd_file_main },
	{ "send", &partadd_send_usage, &partadd_send_main },
	{ "recv", &partadd_recv_usage, &partadd_recv_main },
	{ NULL, NULL, NULL } };

void
partadd_usage(void)
{
	printf("Usage: slsctl partadd <backend> -o <id> [options]\n");

	exit(0);
}

void
partadd_base_usage(char *backend, struct option *opts)
{
	struct option opt;
	int i;

	printf("Full options list for %s:\n", backend);
	for (i = 0, opt = opts[0]; opt.name != NULL; opt = opts[++i])
		printf("-%c\t%s\n", opt.val, opt.name);
}

int
partadd_main(int argc, char *argv[])
{
	int error;
	int i;

	/* Ignore the first argument, which is the partcmd line. */
	argc -= 1;
	argv += 1;

	assert(argc > 0);

	/* Search for the partcmd. */
	for (i = 0; partcmdtable[i].name != NULL; i++) {
		if (strcmp(argv[0], partcmdtable[i].name) != 0)
			continue;

		return partcmdtable[i].cmd(argc, (char **)argv);
	}

	/* No matches. */
	printf("Unrecognized command '%s'\n", argv[0]);

	return (EINVAL);
}
