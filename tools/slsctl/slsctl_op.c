
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option dump_longopts[] = {
	{ "delta", no_argument, NULL, 'd' },
	{ "file", required_argument, NULL, 'f' },
	{ "memory", no_argument, NULL, 'm' },
	{ "pid", required_argument, NULL, 'p' },
	{ NULL, no_argument, NULL, 0 },
};

void
dump_usage(void)
{
    printf("Usage: slsctl dump [-p <PID>] <-f <filename> | -m> [--delta]\n");
}

int
dump_main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd;
	int error;
	int mode;
	int type;
	struct op_param param;
	int type_set;
	int pid_set;
	int opt;
	char *filename;

	param = (struct op_param) { 
		.name = NULL,
		.len = 0,
		.pid = 0,
		.op = SLS_CHECKPOINT,
		.fd_type = SLS_FD_FILE,
		.dump_mode = SLS_FULL,
		.period = 0,
		.iterations = 1,
	};
	pid_set = 0;
	type_set = 0;

	while ((opt = getopt_long(argc, argv, "adf:mp:", dump_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'd':
		param.dump_mode = SLS_DELTA;
		break;
	    case 'f':
		if (type_set == 1) {
		    dump_usage();
		    return 0;
		}

		param.fd_type = SLS_FD_FILE;
		param.name = optarg;
		param.len = strnlen(optarg, 1024);
		truncate(optarg, 0);
		type_set = 1;
		break;
	    case 'm':
		if (type_set == 1) {
		    dump_usage();
		    return 0;
		}

		param.fd_type = SLS_FD_MEM;
		type_set = 1;
		break;
	    case 'p':
		pid_set = 1;
		param.pid = strtol(optarg, NULL, 10);
		break;
	    default:
		dump_usage();
		return 0;
	    }
	}

	if (pid_set == 0) {
	    dump_usage();
	    return 0;
	}

	if (optind != argc) {
	    dump_usage();
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
	int slsmm_fd;
	int error;
	int mode;
	int type;
	int opt;
	int type_set;
	char *filename;
	struct op_param param;

	param = (struct op_param) { 
		.name = NULL,
		.len = 0,
		.pid = getpid(),
		.op = SLS_RESTORE,
		.fd_type = SLS_FD_FILE,
		.dump_mode = SLS_FULL,
		.period = 0,
		.iterations = 0,
	};
	type_set = 0;

	while ((opt = getopt_long(argc, argv, "f:m:", restore_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'f':
		if (type_set == 1) {
		    restore_usage();
		    return 0;
		}

		param.fd_type = SLS_FD_FILE;
		param.name = optarg;
		param.len = strnlen(optarg, 1024);
		break;

	    case 'm':
		if (type_set == 1) {
		    restore_usage();
		    return 0;
		}

		param.fd_type = SLS_FD_MEM;
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
