
#include <stdlib.h>
#include <stdio.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int main() {
    int fd = open("/dev/slsmm", O_RDWR, 0);
    if (fd < 0) {
	perror("open: ");
	exit(1);
    }

    void *mmapped = mmap(NULL, getpagesize(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmapped == MAP_FAILED) {
	perror("mmap: ");
	exit(1);
    }

    char *c = mmapped;
    printf("%c\n", *c);
    *c = 'a';
    printf("%c\n", *c);

    int *d = mmapped;
    printf("%d\n", *d);
    *d = 1234567890;
    printf("%d\n", *d);

    if (msync(mmapped, getpagesize(), MS_SYNC) < 0) {
	perror("msync: ");
    }

    if (munmap(mmapped, getpagesize()) < 0) {
	perror("munmap: ");
    }

    close(fd);
    return 0;
}

