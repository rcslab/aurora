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
    printf("%s %d\n", argv[1], pid);

    int slsmm_fd = open("/dev/slsmm", O_RDWR);
    int file_fd = open(argv[1], O_WRONLY | O_CREAT);

    struct dump_param param = {
        .start  = 0,
        .end    = (vm_offset_t)-1,
        .fd     = file_fd,
        .pid    = pid,
    };

    int error = ioctl(slsmm_fd, SLSMM_DUMP, &param);

    close(slsmm_fd);
    close(file_fd);
    return 0;
}
