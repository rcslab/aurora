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
	printf("Usage: ./vmregion <size in bytes>\n");
	exit(0);
}

void
memsnap(void *addr)
{
	struct timeval ckpt[2];
	int error;

	error = gettimeofday(&ckpt[0], NULL);
	if (error != 0) {
		perror("gettimeofday");
		exit(0);
	}

	error = sls_memsnap(OID, addr);
	if (error != 0) {
		fprintf(stderr, "sls_memsnap returned %d\n", error);
		exit(0);
	}

	error = gettimeofday(&ckpt[1], NULL);
	if (error != 0) {
		perror("gettimeofday");
		exit(0);
	}

	printf("Time elapsed: %ldus\n",
	    (1000 * 1000) * (ckpt[1].tv_sec - ckpt[0].tv_sec) +
		(ckpt[1].tv_usec - ckpt[0].tv_usec));

	sleep(1);
}

int
main(int argc, char *argv[])
{
	struct sls_attr attr;
	uint64_t nextepoch;
	size_t us_elapsed;
	void *startaddr;
	size_t objsize;
	void *mapping;
	int error;
	int i;

	if (argc != 2)
		usage();

	objsize = strtol(argv[1], NULL, 10);
	if (objsize == 0)
		usage();

	mapping = mmap(0x100000000, objsize, PROT_READ | PROT_WRITE,
	    MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);

	if (mapping == NULL) {
		perror("mmap");
		exit(0);
	}

	memset(mapping, random(), objsize);

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		.attr_mode = SLS_DELTA,
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

	error = sls_checkpoint_epoch(OID, false, &nextepoch);
	if (error != 0) {
		fprintf(stderr, "sls_checkpoint returned %d\n", error);
		exit(0);
	}

	error = sls_untilepoch(OID, nextepoch);
	if (error != 0) {
		fprintf(stderr, "sls_untilepoch: %s\n", strerror(error));
		exit(1);
	}

	memset(mapping, 0x5a, objsize);

	memsnap(mapping);

	return (0);
}
