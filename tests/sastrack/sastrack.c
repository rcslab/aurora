#include <sys/mman.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <slos.h>
#include <sls.h>
#include <sls_wal.h>
#include <slsfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KB (1024)
#define MB (1024 * KB)
#define GB (1024 * MB)

#define SIZE (10 * MB)

#define PATH ("%s/sasfile%06d")

/*
 * CAREFUL: this needs to be a divisor of 4096 (PAGE_SIZE)
 * and larger than strnlen(MSG) for the test to work.
 */
#define NUM_MAPPINGS (10)

#define ITERS (128)
#define MAXROUNDS (20)

void *mappings[NUM_MAPPINGS];
int fds[NUM_MAPPINGS];

void
create_mapping(char *name, int index)
{
	const size_t size = SIZE;
	char fullpath[PATH_MAX];
	int error;
	int fd;

	error = slsfs_sas_create(name, size);
	if (error != 0) {
		printf("slsfs_sas_create failed (error %d)\n", error);
		exit(1);
	}

	fd = open(name, O_RDWR, 0666);
	if (fd < 0) {
		perror("open");
		exit(1);
	}
	fds[index] = fd;

	/* Create the first mapping. */
	error = slsfs_sas_map(fd, (void **)&mappings[index]);
	if (error != 0) {
		printf("slsfs_sas_map failed\n");
		exit(1);
	}
}

void
test_tracing(int fd)
{
	size_t rounds = rand() % MAXROUNDS;
	char *addr;
	size_t off;
	int i;

	for (i = 0; i < rounds; i++) {
		addr = (char *)mappings[rand() % NUM_MAPPINGS];
		off = rand() % SIZE;
		addr[off] = 'a' + (rand() % ('z' - 'a'));
	}

	sas_trace_commit(fd);
}

int
main(int argc, char *argv[])
{
	char fullname[PATH_MAX];
	char fileid[PATH_MAX];
	int error;
	int i;

	srand(53);

	for (i = 0; i < NUM_MAPPINGS; i++) {
		snprintf(fullname, PATH_MAX, PATH, argv[1], i);
		create_mapping(fullname, i);
	}

	/* We can use any fd we want. */
	error = sas_trace_start(fds[0]);
	if (error != 0) {
		printf("sas_trace_start failed\n");
		exit(1);
	}

	for (i = 0; i < ITERS; i++)
		test_tracing(fds[0]);

	error = sas_trace_end(fds[0]);
	if (error != 0) {
		printf("sas_trace_end failed\n");
		exit(1);
	}

	return (0);
}
