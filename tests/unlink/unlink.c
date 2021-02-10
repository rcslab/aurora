#include <sys/param.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* The working set size in pages */
#define MEMSIZE (4)
#define FILENAME ("/tmp/unlink")

int
main()
{
	char *array;
	int error;
	int fd;
	int i;

	array = (char *)malloc(MEMSIZE * PAGE_SIZE);
	if (array == NULL) {
		printf("Error: malloc failed\n");
		return 0;
	}

	fd = open(FILENAME, O_CREAT | O_RDWR);
	if (fd < 0) {
		perror("open");
		return 0;
	}

	error = unlink(FILENAME);
	if (error != 0) {
		printf("Error: Unlink failed with %d", error);
		return 0;
	}

	for (;;) {
		for (i = 0; i < MEMSIZE; i++)
			array[i * PAGE_SIZE] = 'a' + (random() % ('z' - 'a'));
	}

	return 0;
}
