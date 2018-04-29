
#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>

int main() {
    int fd = open("/dev/slsmm", O_RDWR, 0);
    int filefd = open("dump.x", O_WRONLY | O_CREAT);
    printf("%d %d\n", fd, filefd);
    ioctl(fd, _IOW('t', 1, int), &filefd);
    return 0;
}
