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
	struct restore_param param;

	if (argc < 3) {
		printf("Usage: procdump <PID> <filenames...>\n");
		return 0;
	}

	pid = strtol(argv[1], &argv[1], 10);
	if (pid < 0) pid = getpid();

	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}

	nfds = argc - 2;
	file_fds = malloc(sizeof(int) * nfds);
	for (int i = 0; i < nfds; i ++) {
		file_fds[i] = open(argv[i+2], O_RDONLY);
		if (!file_fds[i]) {
			printf("ERROR: Checkpoint file not opened\n");
			exit(1); 
		}
	}

	param = (struct restore_param) { 
		.pid = pid, 
		.nfds = nfds,
		.fds = file_fds, 
	};

	ioctl(slsmm_fd, SLSMM_RESTORE, &param);

	return 0;
}
