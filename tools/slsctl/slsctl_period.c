
#include <sys/types.h>
#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option ckptstart_longopts[] = {
	{ "delta", no_argument, NULL, 'd' },
	{ "file", required_argument, NULL, 'f' },
	{ "memory", no_argument, NULL, 'm' },
	{ "iterations", required_argument, NULL, 'n' },
	{ "pid", required_argument, NULL, 'p' },
	{ "period", required_argument, NULL, 't' },
	{ NULL,	no_argument, NULL, 0},
};

void
ckptstart_usage(void)
{
    printf("Usage: slsctl ckptstart -p pid -t <period> [-n iterations]<-f <filename> | -m>[-d]\n");
}

int
ckptstart_main(int argc, char* argv[]) {
	int pid;
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
		.target = SLS_FILE,
		.mode = SLS_FULL,
		.period = 0,
		.iterations = 0,
	};
	pid_set = 0;
	type_set = 0;

	while ((opt = getopt_long(argc, argv, "adf:mn:p:t:", ckptstart_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'd':
		param.mode = SLS_DELTA;
		break;
	    case 'f':
		if (type_set == 1) {
		    ckptstart_usage();
		    return 0;
		}

		param.target = SLS_FILE;
		param.name = optarg;
		param.len = strnlen(optarg, 1024);
		truncate(optarg, 0);
		type_set = 1;
		break;
	    case 'm':
		if (type_set == 1) {
		    ckptstart_usage();
		    return 0;
		}

		param.target = SLS_MEM;
		type_set = 1;
		break;
	    case 'n':
		param.iterations = strtol(optarg, NULL, 10);
		break;
	    case 'p':
		pid_set = 1;
		param.pid = strtol(optarg, NULL, 10);
		break;
	    case 't':
		param.period = strtol(optarg, NULL, 10);
		break;
	    default:
		ckptstart_usage();
		return 0;
	    }
	}

	if (pid_set == 0) {
	    ckptstart_usage();
	    return 0;
	}

	if (param.period == 0) {
	    ckptstart_usage();
	    return 0;
	}

	if (optind != argc) {
	    ckptstart_usage();
	    return 0;
	}


	if (sls_op(&param) < 0)
	    return 1;

	return 0;

}

static struct option ckptstop_longopts[] = {
	{ "pid", required_argument, NULL, 'p' },
	{ NULL,	no_argument, NULL, 0},
};

void
ckptstop_usage(void)
{
	printf("Usage: slsctl ckptstop -p pid\n");
}

int
ckptstop_main(int argc, char* argv[]) {
	int pid;
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
		.op = SLS_CKPT_STOP,
		.target = SLS_FILE,
		.mode = SLS_FULL,
		.period = 0,
		.iterations = 0,
	};
	pid_set = 0;
	type_set = 0;

	while ((opt = getopt_long(argc, argv, "p:", ckptstop_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'p':
		pid_set = 1;
		param.pid = strtol(optarg, NULL, 10);
		break;
	    default:
		ckptstop_usage();
		return 0;
	    }
	}

	if (pid_set == 0) {
	    ckptstop_usage();
	    return 0;
	}

	if (optind != argc) {
	    ckptstop_usage();
	    return 0;
	}


	if (sls_op(&param) < 0)
	    return 1;

	return 0;

}
