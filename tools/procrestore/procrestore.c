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
    if (pid > 0) pid = strtol(argv[2], &argv[2], 10);
    else pid = getpid();

    slsmm_fd = open("/dev/slsmm", O_RDWR);
    if (!slsmm_fd) {
	    printf("ERROR: SLS device file not opened\n");
	    exit(1); 
    }

    file_fd = open(argv[1], O_RDONLY);
    if (!file_fd) {
	    printf("ERROR: Checkpoint file not opened\n");
	    exit(1); 
    }

    param = (struct slsmm_param) { 
	    .fd = file_fd, 
	    .pid = pid, 
    };

    ioctl(slsmm_fd, SLSMM_RESTORE, &param);

    close(file_fd);
    close(slsmm_fd);

    return 0;
}
