#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include "../../kmod/slsmm.h"

//int tmp[8];

int main() {
    int fd = open("/dev/slsmm", O_RDWR, 0);

    int filefd2 = open("../dump1.x", O_RDONLY);
    if (filefd2 < 0) return 0;
    struct restore_req restore_info = {
        .fd = filefd2,
        .pid = -1,
    };
    printf("%d\n", filefd2);
    ioctl(fd, SLSMM_RESTORE, &restore_info);
    printf("done\n");

    close(filefd2);

    return 0;
}
