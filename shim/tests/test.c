#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/param.h>

/* The working set size in pages */
#define MEMSIZE (4)
#define FILENAME ("/tmp/unlink")
#define ERR(err, msg) \
do { \
    if (err < 0) { \
	printf("%s - %s", strerror(errno), msg); \
    } \
}  while(0) \

int main()
{
    char *array;
    int error;
    int fd;
    int i;

    array = (char *) malloc(MEMSIZE * PAGE_SIZE);
    if (array == NULL) {
	printf("Error: malloc failed\n");
	return 0;
    }

    fd = open(FILENAME, O_CREAT | O_RDWR);
    ERR(fd, "open-unlink");
    if (fd < 0) {
	perror("open");
	return 0;
    }
    int fd2 = dup(fd);
    ERR(fd2, "dup");
    int k = dup2(fd, 69);
    ERR(k, "dup2");
    int dir = open("/", O_RDONLY);
    ERR(dir, "open-dir");
    int b = openat(dir, "tmp/foo", O_CREAT | O_RDWR);
    ERR(b, "open-b");
    error = unlink(FILENAME);
    ERR(error, "unlink-unlink");
    error = unlink("/tmp/foo");
    ERR(error, "unlink-foo");
    if (error != 0) {
	printf("Error: Unlink failed with %d", error);
	return 0;
    }
    close(fd);
    close(fd2);
    close(69);
    close(dir);
    close(b);

    return 0;
}
