
#include <stdlib.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main() {
    printf("start\n");
    int fd = open("/dev/slsmm", O_RDWR, 0);
    printf("fd %d\n", fd);
    if (fd < 0) {
        perror("open: ");
        exit(1);
    }

    void *mmapped = mmap(NULL, 4096 * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmapped == MAP_FAILED) {
        perror("mmap: ");
        exit(1);
    }

    char *c = mmapped;
    const char * const d = mmapped + 5000;
    *c = 'a';
    printf("%c\n", *c);
    printf("%c\n", *d);

    printf("%d\n", ioctl(fd, _IOW('t', 4, 0)));

    *c = 'b';
    printf("%c\n", *c);


    close(fd);
    return 0;

    if (msync(mmapped, getpagesize(), MS_SYNC) < 0) {
        perror("msync: ");
    }

    if (munmap(mmapped, getpagesize()) < 0) {
        perror("munmap: ");
    }

    close(fd);
    return 0;
}

