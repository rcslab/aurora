
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <sys/ioctl.h>
#include <fcntl.h>

int tmp;

int main() {
    tmp = 0x12344321;
    int fd = open("/dev/slsmm", O_RDWR, 0);
    int filefd = open("dump.x", O_WRONLY | O_CREAT);
    printf("%d %d\n", fd, filefd);
    ioctl(fd, _IOW('t', 1, int), &filefd);
    //ioctl(fd, _IO('t', 0));
    return 0;
}
