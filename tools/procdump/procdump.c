#include "slsmm.h"

#include <sys/ioctl.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd, file_fd;
	int error;
	struct slsmm_param param;

	if (argc != 3) {
	printf("Usage: procdump <filename> <PID>\n");
		return 0;
	}

	pid = strtol(argv[2], &argv[2], 10); 

	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}

	file_fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC | O_APPEND);
	if (!file_fd) {
		printf("ERROR: Checkpoint file not opened\n");
		exit(1); 
	}

	param = (struct slsmm_param) { 
		.fd = file_fd, 
		.pid = pid, 
	};

	ioctl(slsmm_fd, SLSMM_DUMP, &param);

	close(file_fd);
	close(slsmm_fd);

	return 0;
}
