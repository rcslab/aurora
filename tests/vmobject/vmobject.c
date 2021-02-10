#include <sys/mman.h>
#include <sys/time.h>

#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OID (1000)

void
usage(void)
{
	printf("Usage: ./vmobject <# of objects> <size in bytes>\n");
	exit(0);
}

void
checkpoint_round(int round)
{
	struct timeval ckpt[2];
	int error;

	error = gettimeofday(&ckpt[0], NULL);
	if (error != 0) {
		perror("gettimeofday");
		exit(0);
	}

	error = sls_checkpoint(OID, false);
	if (error != 0) {
		fprintf(stderr, "sls_checkpoint returned %d\n", error);
		exit(0);
	}

	error = gettimeofday(&ckpt[1], NULL);
	if (error != 0) {
		perror("gettimeofday");
		exit(0);
	}

	printf("Time elapsed (%d): %ldus\n", round,
	    (1000 * 1000) * (ckpt[1].tv_sec - ckpt[0].tv_sec) +
		(ckpt[1].tv_usec - ckpt[0].tv_usec));

	sleep(1);
}

int
main(int argc, char *argv[])
{
	size_t objcnt, objsize;
	struct sls_attr attr;
	size_t us_elapsed;
	void **mappings;
	void *startaddr;
	int error;
	int i;

	if (argc != 3)
		usage();

	objcnt = strtol(argv[1], NULL, 10);
	if (objcnt == 0)
		usage();

	objsize = strtol(argv[2], NULL, 10);
	if (objsize == 0)
		usage();

	printf(
	    "Using %lu objects, %lu bytes (or %lu KB, or %lu MB, or %lu GB) each\n",
	    objcnt, objsize, objsize / 1024, objsize / (1024 * 1024),
	    objsize / (1024 * 1024 * 1024));

	mappings = malloc(sizeof(*mappings) * objcnt);
	if (mappings == NULL) {
		perror("malloc");
		exit(0);
	}

	for (i = 0; i < objcnt; i++) {
		mappings[i] = mmap(NULL, objsize, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (mappings[i] == NULL) {
			perror("mmap");
			exit(0);
		}

		memset(mappings[i], random(), objsize);
		if (mmap(mappings[i] + objsize, 0x1000, PROT_NONE, 0, -1, 0) ==
		    NULL) {
			perror("mmap");
			exit(0);
		}
	}

	attr = (struct sls_attr) {
		.attr_target = SLS_MEM,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = SLSATTR_IGNUNLINKED,
	};
	error = sls_partadd(OID, attr);
	if (error != 0) {
		fprintf(stderr, "sls_partadd returned %d\n", error);
		exit(0);
	}

	error = sls_attach(OID, getpid());
	if (error != 0) {
		fprintf(stderr, "sls_attach returned %d\n", error);
		exit(0);
	}

	for (i = 0; i < 2; i++) {
		checkpoint_round(i);
		sleep(3);
	}

	/*
	error = sls_partdel(OID);
	if (error != 0) {
		fprintf(stderr, "sls_partdel returned %d\n", error);
		exit(0);
	}
	*/

	return (0);
}
