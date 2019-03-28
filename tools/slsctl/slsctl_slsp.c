
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option plist_longopts[] = {
	{ NULL, no_argument, NULL, 0 },
};

void
plist_usage(void)
{
    printf("Usage: slsctl plist\n");
}

int
plist_main(int argc, char* argv[])
{
	struct slsp_param param;
	int opt;

	param = (struct slsp_param) {
	    .op = SLS_PLIST,
	    .id = 0,
	};

	while ((opt = getopt_long(argc, argv, "", plist_longopts, NULL)) != -1) {
	    switch (opt) {
	    default:
		plist_usage();
		return 0;
	    }
	}

	if (sls_slsp(&param) < 0)
	    return 1;

	return 0;
}

static struct option pdel_longopts[] = {
	{ "delete", required_argument, NULL, 'd' },
	{ NULL, no_argument, NULL, 0 },
};

void
pdel_usage(void)
{
    printf("Usage: slsctl pdel -d <id>\n");
}

int
pdel_main(int argc, char* argv[])
{
	struct slsp_param param;
	int opt;

	param = (struct slsp_param) {
	    .op = SLS_PDEL,
	    .id = -1,
	};

	while ((opt = getopt_long(argc, argv, "d:", pdel_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'd':
		param.id = strtol(optarg, NULL, 10);
		break;

	    default:
		pdel_usage();
		return 0;
	    }
	}

	if (param.id == -1) {
	    pdel_usage();
	    return 0;
	}

	if (sls_slsp(&param) < 0)
	    return 1;

	return 0;
}
