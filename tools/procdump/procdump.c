#include "slsmm.h"

#include <sys/ioctl.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct option longopts[] = {
	{ "async", no_argument, NULL, 'a' },
	{ "delta", no_argument, NULL, 'd' },
	{ "format", required_argument, NULL, 'f' },
	{ "pid", required_argument, NULL, 'p' },
	{ NULL, no_argument, NULL, 0 },
};

void
usage(void)
{
    printf("Usage: procdump <-p|--pid> <PID> [<-f | --format> <file <filename> | memory | osd>] [--delta] [--async]\n");
}

int main(int argc, char* argv[]) {
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

	while ((opt = getopt_long(argc, argv, "adf:p:", longopts, NULL)) != -1) {
	    switch (opt) {
	    case 'a':
		param.async = SLSMM_CKPT_ASYNC;
		break;

	    case 'd':
		param.dump_mode = SLSMM_CKPT_DELTA;
		break;

	    case 'f':
		if (strcmp(optarg, "file") == 0)
		    param.fd_type = SLSMM_FD_FILE; 
		else if (strcmp(optarg, "memory") == 0)
		    param.fd_type = SLSMM_FD_MEM; 
		else if (strcmp(optarg, "osd") == 0)
		    param.fd_type = SLSMM_FD_NVDIMM; 
		else 
		    printf("Invalid output type, defaulting to file\n");
		break;

	    case 'p':
		pid_set = 1;
		param.pid = strtol(optarg, NULL, 10);
		break;

	    default:
		usage();
		return 0;
	    }
	}

	if (pid_set == 0) {
	    usage();
	    return 0;
	}

	if (optind == argc - 1) {
	    if (param.fd_type != SLSMM_FD_FILE) {
		usage();
		return 0;
	    }

	    filename = argv[optind];
	    param.name = filename;
	    param.len = strnlen(filename, 1024);

	    truncate(filename, 0);
	} else if (param.fd_type == SLSMM_FD_FILE) {
	    usage();
	    return 0;
	}


	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}

	ioctl(slsmm_fd, SLSMM_DUMP, &param);

	close(slsmm_fd);

	return 0;

}
