#include <sys/mman.h>

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define min(a, b) ((a < b) ? (a) : (b))

#define PAGE_SIZE (4096)
#define FILE_SIZE (PAGE_SIZE * 64)

void
testoffset(int fd, int start)
{
	int i, j, newi;
	int res;

	assert(start % sizeof(newi) == 0);

	/* Read the offsets back, make sure they're correct. */
	for (i = start; i < FILE_SIZE; i += sizeof(i)) {
		res = 0;
		for (j = 0; j < sizeof(i); j += res) {
			res = read(fd, ((char *)&newi) + res, sizeof(newi) - j);
			if (res < 0) {
				perror("read");
				exit(1);
			}
		}
		if (i != newi) {
			printf("Read %d instead of %d\n", newi, i);
			exit(1);
		}
	}
}

int
main(int argc, char **argv)
{
	int error;
	int res;
	int i, j;
	int fd;

	if (argc != 2) {
		printf("Usage:./fd basedir\n");
		exit(1);
	}

	error = chdir(argv[1]);
	if (error < 0) {
		perror("chdir");
		exit(1);
	}

	fd = open("testfile", O_RDWR | O_TRUNC | O_CREAT);
	if (fd < 0) {
		printf("Error: open failed\n");
		exit(1);
	}

	/* Write the file offsets in the offsets themselves. */
	for (i = 0; i < FILE_SIZE; i += sizeof(i)) {
		res = 0;
		for (j = 0; j < sizeof(i); j += res) {
			res = write(fd, ((char *)&i) + res, sizeof(i));
			if (res < 0) {
				perror("write");
				exit(1);
			}
		}
	}

	/* Make sure the middle of the file is the start of a valid offset. */
	assert((FILE_SIZE / 2) % sizeof(i) == 0);

	/* Seek to the middle, when we restore we retain the offset.  */
	error = lseek(fd, FILE_SIZE / 2, SEEK_SET);
	if (error < 0) {
		perror("lseek");
		exit(1);
	}

	sleep(2);

	testoffset(fd, FILE_SIZE / 2);

	/* Start from the beginning, make sure everything is alright. */
	error = lseek(fd, 0, SEEK_SET);
	if (error < 0) {
		perror("lseek");
		exit(1);
	}

	testoffset(fd, 0);

	return (0);
}
