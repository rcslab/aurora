#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/time.h>

#include <sls.h>

#define OID (1000)

void
usage(void)
{
	printf("Usage: ./vmobject <# of objects> <size in 4KB pages>\n");
	exit(0);
}

int
main(int argc, char *argv[])
{
	struct timeval ckptstart, ckptend;
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
	objsize *= 4096;

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
		if (mmap(mappings[i] + objsize, 0x1000, PROT_NONE, 0, -1, 0) == NULL) {
			perror("mmap");
			exit(0);
		}
	}

	attr = (struct sls_attr) {
		.attr_target = SLS_MEM,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
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

	error = gettimeofday(&ckptstart, NULL);
	if (error != 0) {
		perror("gettimeofday");
		exit(0);
	}

	error = sls_checkpoint(OID, false, true);
	if (error != 0) {
		fprintf(stderr, "sls_checkpoint returned %d\n", error);
		exit(0);
	}

	error = gettimeofday(&ckptend, NULL);
	if (error != 0) {
		perror("gettimeofday");
		exit(0);
	}

	printf("Time elapsed: %ldus\n", (1000 * 1000) * 
			(ckptend.tv_sec - ckptstart.tv_sec) + (ckptend.tv_usec - ckptstart.tv_usec));

	/*
	error = sls_partdel(OID);
	if (error != 0) {
		fprintf(stderr, "sls_partdel returned %d\n", error);
		exit(0);
	}
	*/

	return (0);
}
