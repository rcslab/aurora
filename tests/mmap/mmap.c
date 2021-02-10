#include <sys/param.h>
#include <sys/mman.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MMAP_SIZE (PAGE_SIZE * 64)

static int
mmap_file(void **addr)
{
	void *mapping;
	int error;
	int fd;

	fd = open("testfile", O_RDWR | O_TRUNC | O_CREAT, 0666);
	if (fd < 0) {
		perror("open");
		close(fd);
		return (-1);
	}

	/* Make sure the file is large enough. */
	error = lseek(fd, MMAP_SIZE - 1, SEEK_SET);
	assert(error > 0);

	error = write(fd, "", 1);
	assert(error >= 0);

	mapping = mmap(
	    NULL, MMAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return (-1);
	}

	*addr = mapping;

	close(fd);
	return (0);
}

static int
mmap_anon(void **addr)
{
	void *mapping;

	mapping = mmap(NULL, MMAP_SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return (-1);
	}

	*addr = mapping;
	return (0);
}

int
main(int argc, char **argv)
{
	int error;
	void *mapping;
	int i;

	if (argc != 3) {
		printf("Usage:./mmap basedir <anon|file>\n");
		exit(1);
	}

	error = chdir(argv[1]);
	if (error < 0) {
		perror("chdir");
		exit(1);
	}

	if (strcmp(argv[2], "anon") == 0)
		error = mmap_anon(&mapping);
	else if (strcmp(argv[2], "file") == 0)
		error = mmap_file(&mapping);
	else
		error = EINVAL;

	if (error != 0) {
		printf("Error %d\n", error);
		exit(1);
	}

	memset(mapping, 'a', MMAP_SIZE);

	sleep(5);

	for (i = 0; i < MMAP_SIZE; i++) {
		if (((char *)mapping)[i] != 'a') {
			printf("Error\n");
			exit(1);
		}
	}

	printf("Success\n");

	return (0);
}
