#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/param.h>
#include <sys/mman.h>

#define FILE_SIZE (PAGE_SIZE * 64)


int
main(int argc, char **argv)
{
	int error;
	void *file_mapping;
	int i;
	int fd;

	/* This file is unlinked, so this test should be called in the SLOS. */
	fd = open("testfile", O_RDWR | O_TRUNC | O_CREAT, 0666);
	if (fd < 0) {
		printf("Error: open failed\n");
		return -1;
	}

	/* Make sure the file is large enough*/
	error = lseek(fd, FILE_SIZE - 1, SEEK_SET);
	assert(error > 0);

	error = write(fd, "", 1);
	assert(error >= 0);

	file_mapping = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (file_mapping == MAP_FAILED) {
		printf("Error: mmap failed\n");
		return -1;
	}

	memset(file_mapping, 'a', FILE_SIZE);

	sleep(200);

	for (i = 0; i < FILE_SIZE; i++) {
		if (((char *) file_mapping)[i] != 'a') {
			printf("Error\n");
			exit(1);
		}

	}

	sleep(5);
	printf("Success\n");

	return (0);
}
