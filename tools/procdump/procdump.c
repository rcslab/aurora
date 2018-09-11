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
	int mode;
	struct dump_param param;

	if (argc != 4) {
		printf("Usage: procdump <filename> <PID> <Mode>\n");
		return 0;
	}

	pid = strtol(argv[2], &argv[2], 10); 
	mode = strtol(argv[3], &argv[3], 10); 

	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}

	file_fd = open(argv[1], O_WRONLY | O_CREAT | O_APPEND | O_TRUNC);
	if (!file_fd) {
		printf("ERROR: Checkpoint file not opened\n");
		exit(1); 
	}

	param = (struct dump_param) { 
		.fd = file_fd, 
		.pid = pid, 
	};

	switch (mode) {
		case SLSMM_CKPT_FULL:
			ioctl(slsmm_fd, FULL_DUMP, &param);
			break;

		case SLSMM_CKPT_DELTA:
			ioctl(slsmm_fd, DELTA_DUMP, &param);
			break;
	}

	close(file_fd);
	close(slsmm_fd);

	return 0;
}
