
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
	{ "async", no_argument, NULL, 'a' },
	{ "delta", no_argument, NULL, 'd' },
	{ "format", required_argument, NULL, 'f' },
	{ "pid", required_argument, NULL, 'p' },
	{ NULL, no_argument, NULL, 0 },
};

void
dump_usage(void)
{
    printf("Usage: slsctl dump [-p <PID>] [<-f <file <filename> | memory | osd>] [--delta] [--async]\n");
}

int
dump_main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd;
	int error;
	int mode;
	int type;
	int async;
	struct dump_param param;
	int pid_set;
	int opt;
	char *filename;

	param = (struct dump_param) { 
		.name = NULL,
		.len = 0,
		.pid = 0,
		.fd_type = SLSMM_FD_FILE,
		.dump_mode = SLSMM_CKPT_FULL,
		.async = SLSMM_CKPT_SYNC,
	};
	pid_set = 0;

	while ((opt = getopt_long(argc, argv, "adf:p:", dump_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'a':
		param.async = SLSMM_CKPT_ASYNC;
		break;
	    case 'd':
		param.dump_mode = SLSMM_CKPT_DELTA;
		break;
	    case 'f':
		if (strcmp(optarg, "file") == 0) {
		    param.fd_type = SLSMM_FD_FILE; 
		} else if (strcmp(optarg, "memory") == 0) {
		    param.fd_type = SLSMM_FD_MEM; 
		} else if (strcmp(optarg, "osd") == 0) {
		    param.fd_type = SLSMM_FD_NVDIMM; 
		} else {
		    printf("Invalid output type\n");
		    dump_usage();
		    return 1;
		}
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

	if (optind == argc - 1) {
	    if (param.fd_type != SLSMM_FD_FILE) {
		dump_usage();
		return 0;
	    }

	    filename = argv[optind];
	    param.name = filename;
	    param.len = strnlen(filename, 1024);

	    truncate(filename, 0);
	} else if (param.fd_type == SLSMM_FD_FILE) {
	    dump_usage();
	    return 0;
	}


	if (sls_dump(&param) < 0)
	    return 1;

	return 0;

}

static struct option restore_longopts[] = {
	{ "format", required_argument, NULL, 'f' },
	{ NULL, no_argument, NULL, 0 },
};

void
restore_usage(void)
{
	printf("Usage: slsctl restore [-f <file | memory | osd>] [FILENAME] \n");
}

int
restore_main(int argc, char* argv[]) {
	int slsmm_fd;
	int error;
	int mode;
	int type;
	int opt;
	char *filename;
	struct restore_param param;

	printf("Warning: Only files can be used for restoring right now\n");
	param = (struct restore_param) { 
		.name = NULL,
		.len = 0,
		.pid = getpid(),
		.fd_type = SLSMM_FD_FILE,
	};

	while ((opt = getopt_long(argc, argv, "f:", restore_longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'f':
		if (strcmp(optarg, "file") == 0) {
		    param.fd_type = SLSMM_FD_FILE; 
		} else if (strcmp(optarg, "memory") == 0) {
		    param.fd_type = SLSMM_FD_MEM; 
		} else if (strcmp(optarg, "osd") == 0) {
		    param.fd_type = SLSMM_FD_NVDIMM; 
		} else {
		    printf("Invalid output type\n");
		    restore_usage();
		    return 0;
		}
		break;
	    default:
		restore_usage();
		return 0;
	    }
	}

	if (optind == argc - 1) {
	    if (param.fd_type != SLSMM_FD_FILE) {
		restore_usage();
		return 0;
	    }

	    filename = argv[optind];
	    param.name = filename;
	    param.len = strnlen(filename, 1024);
	} else if (param.fd_type == SLSMM_FD_FILE) {
	    restore_usage();
	    return 0;
	}

	if (sls_restore(&param) < 0)
	    return 1;

	return 0;
}

void
usage(void)
{
	printf("Usage: slsctl <command> [command args]\n");
	printf("       slsctl dump      [-p <PID>][-f <file <filename> | memory | osd>][-d][-a]\n");
	printf("       slsctl restore   [-f <file filename | memory | osd>\n");
}

struct command {
	const char *name;
	void(*usage)();
	int(*cmd)(int, char **);
} commandtable[] = {
	{ "dump",	&dump_usage,		&dump_main },
	{ "restore",	&restore_usage,		&restore_main },
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
			return commandtable[i].cmd(argc - 1, argv + 1);
	}

	return 0;
}

