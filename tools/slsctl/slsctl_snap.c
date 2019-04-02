
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option snaplist_longopts[] = {
	{ NULL, no_argument, NULL, 0 },
};

void
snaplist_usage(void)
{
    printf("Usage: slsctl snaplist\n");
}

int
snaplist_main(int argc, char* argv[])
{
	struct snap_param param;
	int opt;

	param = (struct snap_param) {
	    .op = SLS_SNAPLIST,
	    .id = 0,
	};

	while ((opt = getopt_long(argc, argv, "", snaplist_longopts, NULL)) != -1) {
	    switch (opt) {
	    default:
		snaplist_usage();
		return 0;
	    }
	}

	if (sls_snap(&param) < 0)
	    return 1;

	return 0;
}

static struct option snapdel_longopts[] = {
	{ "delete", required_argument, NULL, 'd' },
	{ NULL, no_argument, NULL, 0 },
};

void
snapdel_usage(void)
{
    printf("Usage: slsctl snapdel -d <id>\n");
}

int
snapdel_main(int argc, char* argv[])
{
	struct snap_param param;
	int opt;

	param = (struct snap_param) {
	    .op = SLS_SNAPDEL,
	    .id = -1,
	};

	while ((opt = getopt_long(argc, argv, "d:", snapdel_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'd':
		param.id = strtol(optarg, NULL, 10);
		break;

	    default:
		snapdel_usage();
		return 0;
	    }
	}

	if (param.id == -1) {
	    snapdel_usage();
	    return 0;
	}

	if (sls_snap(&param) < 0)
	    return 1;

	return 0;
}
