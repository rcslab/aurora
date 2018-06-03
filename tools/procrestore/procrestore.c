#include "slsmm.h"

#include <sys/ioctl.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        return 0;
    }

    int pid = strtol(argv[2], &argv[2], 10);

    int slsmm_fd = open("/dev/slsmm", O_RDWR);
    int file_fd = open(argv[1], O_RDONLY);
    printf("%d\n", file_fd);

    struct restore_req param = {
        .fd     = file_fd,
        .pid    = pid,
    };

    int error = ioctl(slsmm_fd, SLSMM_RESTORE, &param);
    printf("resume error %d\n", error);

    close(slsmm_fd);
    close(file_fd);
    return 0;
}
