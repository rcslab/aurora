#include "slsmm.h"

#include <sys/ioctl.h>
#include <sys/time.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ITERATIONS (1000)
#define PERIOD (10000)

int main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd, file_fd = 0;
	int error;
	int mode;
	int type;
	struct dump_param param;
    int i;
    useconds_t duration;
    struct timeval start, end;

	if (argc != 4 && argc != 3) {
		printf("Usage: stressdump <filename> <PID> <Mode>\n");
		printf("Usage: stressump <PID> <Mode>\n");
		return 0;
	}

	pid = strtol(argv[argc-2], &argv[argc-2], 10); 
	mode = strtol(argv[argc-1], &argv[argc-1], 10); 
	type = argc == 4 ? SLSMM_FD_FILE : SLSMM_FD_MEM;

	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}

	if (type == SLSMM_FD_FILE) {
		file_fd = open(argv[1], O_WRONLY | O_CREAT | O_APPEND | O_TRUNC);
		if (!file_fd) {
			printf("ERROR: Checkpoint file not opened\n");
			exit(1); 
		}
	}

	param = (struct dump_param) { 
		.fd = file_fd, 
		.pid = pid, 
		.fd_type = type,
	};

    for (i = 0; i < ITERATIONS; i++) {
        gettimeofday(&start, NULL);
        switch (mode) {
            
            case SLSMM_CKPT_FULL:
                ioctl(slsmm_fd, FULL_DUMP, &param);
                break;

            case SLSMM_CKPT_DELTA:
                ioctl(slsmm_fd, DELTA_DUMP, &param);
                break;
        }
        gettimeofday(&end, NULL);
        duration = (1000 * 1000 * (end.tv_sec - start.tv_sec)) + 
                        (end.tv_usec - start.tv_usec);
        usleep(PERIOD - duration);
    }

	if (type == SLSMM_FD_MEM) 
		printf("memory descriptor: %d\n", param.fd);

	close(file_fd);
	close(slsmm_fd);

	return 0;
}
