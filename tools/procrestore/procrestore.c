#include "slsmm.h"

#include <sys/ioctl.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd, *file_fds;
	int nfds;
	int error;
	int type;
	int fd;
	struct restore_param param;
	char **fds;

	if (argc < 3) {
		printf("Usage: procdump <type> <filenames...>\n");
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

	/* The fds fds now an array of fds of size nfds */
	nfds = argc - 2;
	fds = &argv[2];

	file_fds = malloc(sizeof(int) * nfds);
	for (int i = 0; i < nfds; i ++) {

		if (type == SLSMM_FD_FILE) {
			fd = open(fds[i], O_RDONLY);
			if (fd < 0) {
				perror("open");
				exit(1);
			}
		}
		else {
			fd = strtol(fds[i], &fds[i], 10);
			if (!fd) {
				printf("Error: Invalid mem descriptor %s\n", fds[i]);
				exit(1);
			}
		}

		file_fds[i] = fd;
	}

	param = (struct restore_param) { 
		.nfds = nfds,
		.fds = file_fds, 
		.fd_type = type,
	};

	ioctl(slsmm_fd, SLSMM_RESTORE, &param);

	return 0;
}
