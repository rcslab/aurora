
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <fcntl.h>

//int tmp[8];

int main() {
    int *tmp = malloc(sizeof(int)*128);
    memset(tmp, 0x3f, sizeof(int)*128);
    printf("%lx\n", (void*)tmp);
    int fd = open("/dev/slsmm", O_RDWR, 0);
    int filefd = open("dump.x", O_WRONLY | O_CREAT);
    printf("%d %d\n", fd, filefd);
    ioctl(fd, _IOW('t', 1, int), &filefd);
    //ioctl(fd, _IO('t', 0));
    return 0;
}
