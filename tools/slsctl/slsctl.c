
#include <sys/types.h>
#include <sys/ioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slsctl.h"

void
usage(void)
{
	printf("Usage: slsctl <command> [command args]\n");
	printf("       slsctl checkpoint -p <PID> \n");
	printf("       slsctl restore <-f <filename> | -m id>>\n");
	printf("       slsctl attach -p pid -t <period> [-n iterations] <-f <filename> | -m> [-d]\n");
	printf("       slsctl detach -p pid\n");
}

struct command {
	const char *name;
	void(*usage)();
	int(*cmd)(int, char **);
} commandtable[] = {
	{ "checkpoint",	&checkpoint_usage,  &checkpoint_main },
	{ "restore",	&restore_usage,	    &restore_main },
	{ "attach",	&attach_usage,	    &attach_main},
	{ "detach",	&detach_usage,      &detach_main},
	{ NULL,		NULL,		    NULL }
};

int
main(int argc, const char *argv[])
{
	if (argc < 2) {
		usage();
		exit(0);
	}

	for (int i = 0; commandtable[i].name != NULL; i++) {
		if (strcmp(argv[1], commandtable[i].name) == 0)
			return commandtable[i].cmd(argc - 1, (char **) argv + 1);
	}

	return 0;
}

