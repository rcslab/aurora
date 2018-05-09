
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include "../kmod/slsmm.h"

//int tmp[8];

int main() {
    int *tmp = malloc(sizeof(int)*128);
    printf("%lx\n", (void*)tmp);
    int fd = open("/dev/slsmm", O_RDWR, 0);
    int filefd1 = open("dump1.x", O_WRONLY | O_CREAT);
    int pid;
    scanf("%d", &pid);

    /*
    memset(tmp, 0x3f, sizeof(int)*128);
    ioctl(fd, SLSMM_DUMP, &filefd1);
    printf("%x\n", tmp[123]);

    memset(tmp, 0x4f, sizeof(int)*128);
    ioctl(fd, SLSMM_DUMP, &filefd2);
    printf("%x\n", tmp[123]);
    */

    memset(tmp, 0x3f, sizeof(int)*128);
    struct dump_range_req info = {
        .start  = 0,//(vm_offset_t)tmp,
        .end    = (vm_offset_t)-1,//(vm_offset_t)tmp+sizeof(int)*128,
        .fd     = filefd1,
        .pid    = pid,
    };
    printf("%d\n", info.pid);
    printf("%lx %lx\n", info.start, info.end);
    ioctl(fd, SLSMM_DUMP_RANGE, &info);
    printf("done\n");

    return 0;
}
