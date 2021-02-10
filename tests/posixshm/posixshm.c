#include <sys/types.h>
#include <sys/mman.h>

#include <machine/atomic.h>

#include <fcntl.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define POSIXPATH ("/fake/path/posixshm")
#define SHM_SIZE (4096)

int
main()
{
	key_t key;
	pid_t pid;
	char *shm;
	uint8_t *mtx;
	int error, i;
	int fd;
	char c;

	fd = shm_open(POSIXPATH, O_RDWR | O_CREAT, 0666);
	if (fd < 0) {
		perror("shm_open");
		exit(1);
	}

	shm_unlink(POSIXPATH);

	error = ftruncate(fd, getpagesize());
	if (error != 0) {
		perror("ftruncate");
		exit(1);
	}

	shm = (char *)mmap(
	    NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	c = 'a' + (random() % ('z' - 'a' + 1));
	memset(shm, c, SHM_SIZE);
	munmap(shm, SHM_SIZE);

	sleep(5);

	shm = (char *)mmap(
	    NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}

	for (i = 0; i < SHM_SIZE; i++) {
		if (shm[i] != c) {
			printf("Found byte '%d' instead of %d", shm[i], c);
			exit(1);
		}
	}

	close(fd);
	printf("Done.\n");

	return 0;
}
