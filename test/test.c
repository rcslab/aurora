
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
    close(filefd1);

    printf("done\n");
    memset(tmp, 0x4f, sizeof(int)*128);

    int filefd2 = open("dump1.x", O_RDONLY);
    if (filefd2 < 0) return 0;
    struct restore_req restore_info = {
        .fd = filefd2,
        .pid = pid,
    };
    printf("%d\n", filefd2);
    ioctl(fd, SLSMM_RESTORE, &restore_info);
    printf("done\n");

    printf("%x\n", tmp[0]);

    close(filefd2);
    free(tmp);

    return 0;
}
