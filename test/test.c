
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <fcntl.h>

//int tmp[8];

int main() {
    int *tmp = malloc(sizeof(int)*128);
    printf("%lx\n", (void*)tmp);
    int fd = open("/dev/slsmm", O_RDWR, 0);
    int filefd1 = open("dump1.x", O_WRONLY | O_CREAT);
    int filefd2 = open("dump2.x", O_WRONLY | O_CREAT);

    for (int i = 0; i < 100; i ++) {
    memset(tmp, 0x3f, sizeof(int)*128);
    ioctl(fd, _IOW('t', 1, int), &filefd1);
    printf("%x\n", tmp[123]);

    memset(tmp, 0x4f, sizeof(int)*128);
    ioctl(fd, _IOW('t', 1, int), &filefd2);
    printf("%x\n", tmp[123]);
    }

    return 0;
}
