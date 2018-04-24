
#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>

int main() {
    int fd = open("/dev/slsmm", O_RDWR, 0);
    ioctl(fd, _IO('t', 1));
    return 0;
}
