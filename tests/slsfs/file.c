#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main()
{
	int error;

	int fd = open("/testmnt/test.txt", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	ssize_t size = pwrite(fd, "hello", 5, 10000);
	if (size == -1) {
		printf("%s - uh oh", strerror(errno));
		return 1;
	}
	if (size != 5) {
		printf("%s - uh oh", strerror(errno));
		return 1;
	}
	char ding[255];
	size = pread(fd, ding, 255, 0);
	printf("%zd read\n", size);
	if (size == -1) {
		printf("%s - uh oh", strerror(errno));
		return -1;
	}
	ding[size + 1] = '\0';
	printf("I read: %s\n", ding);
	close(fd);
	return 0;
}
