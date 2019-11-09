
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
	printf("Commands: partadd partdel attach checkpoint restore\n");
}

struct command {
	const char *name;
	void(*usage)();
	int(*cmd)(int, char **);
} commandtable[] = {
	{ "partadd",	&partadd_usage,	    &partadd_main},
	{ "partdel",	&partdel_usage,	    &partdel_main},
	{ "attach",	&attach_usage,	    &attach_main},
	{ "checkpoint",	&checkpoint_usage,  &checkpoint_main },
	{ "restore",	&restore_usage,	    &restore_main },
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

