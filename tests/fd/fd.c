#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#define min(a, b) ((a < b) ? (a) : (b))

#define PAGE_SIZE (4096)
#define FILE_SIZE (PAGE_SIZE * 64)


const char value[] = "0xdeadbeef";

int
main(int argc, char **argv)
{
	int error;
	int res;
	int i, j;
	int fd;

	if (argc != 2) {
		printf("Usage: ./fd <file>\n");
		return 0;
	}

	fd = open(argv[1], O_RDWR | O_TRUNC | O_CREAT);
	if (fd < 0) {
		printf("Error: open failed\n");
		return -1;
	}

	printf("fd is %d\n", fd);
	printf("Press any key to copy...\n");
	getchar();
	sleep(10);
	
	for (i = 0; i < FILE_SIZE; i += sizeof(value)) {
		res = 0;
		for (j = 0; j < sizeof(value); j += res) {
			res = write(fd, value, sizeof(value));
			if (res < 0) {
				perror("write");
				goto out;
			}
		}
	}

out:
	printf("Press any key to exit...\n");
	getchar();
	
	return 0;
}
