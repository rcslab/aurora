
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option checkpoint_longopts[] = {
	{ "delta", no_argument, NULL, 'd' },
	{ "file", required_argument, NULL, 'f' },
	{ "memory", no_argument, NULL, 'm' },
	{ "pid", required_argument, NULL, 'p' },
	{ NULL, no_argument, NULL, 0 },
};

void
checkpoint_usage(void)
{
    printf("Usage: slsctl checkpoint [-p <PID>] <-f <filename> | -m> [--delta]\n");
}

int
checkpoint_main(int argc, char* argv[]) {
	int pid;
	int error;
	int mode;
	int target;
	struct op_param param;
	int target_set;
	int pid_set;
	int opt;
	char *filename;

	param = (struct op_param) { 
		.name = NULL,
		.len = 0,
		.pid = 0,
		.op = SLS_CHECKPOINT,
		.target = SLS_FILE,
		.mode = SLS_FULL,
		.period = 0,
		.iterations = 1,
	};
	pid_set = 0;
	target_set = 0;

	while ((opt = getopt_long(argc, argv, "adf:mp:", checkpoint_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'd':
		param.mode = SLS_DELTA;
		break;
	    case 'f':
		if (target_set == 1) {
		    checkpoint_usage();
		    return 0;
		}

		param.target = SLS_FILE;
		param.name = optarg;
		param.len = strnlen(optarg, 1024);
		truncate(optarg, 0);
		target_set = 1;
		break;
	    case 'm':
		if (target_set == 1) {
		    checkpoint_usage();
		    return 0;
		}

		param.target = SLS_MEM;
		target_set = 1;
		break;
	    case 'p':
		pid_set = 1;
		param.pid = strtol(optarg, NULL, 10);
		break;
	    default:
		checkpoint_usage();
		return 0;
	    }
	}

	if (pid_set == 0) {
	    checkpoint_usage();
	    return 0;
	}

	if (optind != argc) {
	    checkpoint_usage();
	    return 0;
	}


	if (sls_op(&param) < 0)
	    return 1;

	return 0;

}

static struct option restore_longopts[] = {
	{ "file", required_argument, NULL, 'f' },
	{ "memory", required_argument, NULL, 'm' },
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore <-f <filename> | -m <id>> \n");
}

int
restore_main(int argc, char* argv[]) {
	int error;
	int mode;
	int target;
	int opt;
	int target_set;
	char *filename;
	struct op_param param;

	param = (struct op_param) { 
		.name = NULL,
		.len = 0,
		.pid = getpid(),
		.op = SLS_RESTORE,
		.target = SLS_FILE,
		.mode = SLS_FULL,
		.period = 0,
		.iterations = 0,
	};
	target_set = 0;

	while ((opt = getopt_long(argc, argv, "f:m:", restore_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'f':
		if (target_set == 1) {
		    restore_usage();
		    return 0;
		}

		param.target = SLS_FILE;
		param.name = optarg;
		param.len = strnlen(optarg, 1024);
		break;

	    case 'm':
		if (target_set == 1) {
		    restore_usage();
		    return 0;
		}

		param.target = SLS_MEM;
		param.id = strtol(optarg, NULL, 10);
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

	if (sls_op(&param) < 0)
	    return 1;

	return 0;
}
