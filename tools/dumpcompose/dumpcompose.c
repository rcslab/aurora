#include "slsmm.h"

#include <sys/ioctl.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd, *fds;
	int nfds;
	int error;
	int type, outtype;
	int fd, outfd;
	struct compose_param param;
	char **filenames, *outfilename;

	if (argc < 3) {
		printf("Usage: procdump <outtype> <outname> <type> <filenames...>\n");
		return 0;
	}

	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}


	type = atoi(argv[1]);
	if (type <= SLSMM_FD_INVALID_LOW || type >= SLSMM_FD_INVALID_HIGH) {
		printf("ERROR: Invalid checkpointing type (1 - file, 2 - memory)\n");
		exit(1);
	}

	outtype = atoi(argv[3]);
	if (outtype <= SLSMM_FD_INVALID_LOW || outtype >= SLSMM_FD_INVALID_HIGH) {
		printf("ERROR: Invalid checkpointing type (1 - file, 2 - memory)\n");
		exit(1);
	}

	/* The fds fds now an array of fds of size nfds */
	outfilename = argv[2];
	filenames = &argv[4];
	nfds = argc - 4;

	if (type == SLSMM_FD_FILE) {
		outfd = open(outfilename, O_RDONLY);
		if (outfd < 0) {
			perror("open");
			exit(1);
		}
	}
	else {
		printf("Error: Non-file arguments temporarily unsupported\n");
		exit(1);
	}

	fds = malloc(sizeof(int) * nfds);
	for (int i = 0; i < nfds; i ++) {
		if (type == SLSMM_FD_FILE) {
			fd = open(filenames[i], O_RDONLY);
			if (fd < 0) {
				perror("open");
				exit(1);
			}
		}
		else {
			printf("Error: Non-file arguments temporarily unsupported\n");
			exit(1);
		}

		fds[i] = fd;
	}

	param = (struct compose_param) { 
		.nfds = nfds,
		.fds = fds, 
		.fd_type_in = type,
		.outfd = outtype,
		.fd_type_out = outfd,
	};

	ioctl(slsmm_fd, SLSMM_COMPOSE, &param);

	return 0;
}
