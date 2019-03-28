
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

#include "slsctl.h"

void
usage(void)
{
	printf("Usage: slsctl <command> [command args]\n");
	printf("       slsctl dump      -p <PID> <-f <filename> | -m>[-d]\n");
	printf("       slsctl restore   <-f <filename> | -m id>>\n");
	printf("       slsctl plist\n");
	printf("       slsctl pdel -d <id>\n");
	printf("       slsctl ckptstart -p pid -t <period> [-n iterations]<-f <filename> | -m>[-d]\n");
	printf("       slsctl ckptstop -p pid\n");
}

struct command {
	const char *name;
	void(*usage)();
	int(*cmd)(int, char **);
} commandtable[] = {
	{ "dump",	&dump_usage,		&dump_main },
	{ "restore",	&restore_usage,		&restore_main },
	{ "plist",	&plist_usage,		&plist_main},
	{ "pdel",	&pdel_usage,		&pdel_main},
	{ "ckptstart",	&ckptstart_usage,	&ckptstart_main},
	{ "ckptstop",	&ckptstop_usage,	&ckptstop_main},
	{ NULL,		NULL,			NULL }
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
