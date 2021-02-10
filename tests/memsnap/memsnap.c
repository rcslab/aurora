#include <sys/param.h>
#include <sys/mman.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MMAP_SIZE (PAGE_SIZE * 64)

static int
mmap_anon(void **addr)
{
	void *mapping;

	mapping = mmap((void *)0x100000000, MMAP_SIZE, PROT_READ | PROT_WRITE,
	    MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
	if (mapping == MAP_FAILED) {
		perror("mmap");
		return (-1);
	}

	printf("Mapping at %p\n", mapping);

	*addr = mapping;
	return (0);
}

static bool
mmap_isfilled(void *addr, char c)
{
	int i;

	for (i = 0; i < MMAP_SIZE; i++) {
		if (((char *)addr)[i] != c) {
			return (false);
		}
	}

	return (true);
}

static struct option memsnap_longopts[] = {
	{ "poll", no_argument, NULL, 'p' },
	{ "memory", no_argument, NULL, 'm' },
	{ "wait", no_argument, NULL, 'w' },
	{ NULL, no_argument, NULL, 0 },
};

enum ckptwait {
	NOWAIT,
	BUSYWAIT,
	BLOCK,
};

static void
wait_for_sls(uint64_t nextepoch, uint64_t oid, enum ckptwait ckptwait)
{
	bool isdone;
	int error;

	switch (ckptwait) {
	case NOWAIT:
		/* Nothing to do. */
		break;

	case BUSYWAIT:
		do {
			usleep(10);
			error = sls_epochdone(oid, nextepoch, &isdone);
			if (error != 0) {
				fprintf(stderr, "sls_epochdone: %s\n",
				    strerror(error));
				exit(1);
			}
		} while (!isdone);

		break;

	case BLOCK:

		error = sls_untilepoch(oid, nextepoch);
		if (error != 0) {
			fprintf(
			    stderr, "sls_untilepoch: %s\n", strerror(error));
			exit(1);
		}

		break;

	default:
		fprintf(stderr, "Invalid enum ckptwait %d\n", ckptwait);
		exit(1);
		break;
	}
}

int
main(int argc, char **argv)
{
	enum ckptwait wait;
	uint64_t nextepoch;
	void *addr;
	int error;
	uint64_t oid;
	int opt;

	/* Create the anonymous mapping*/
	error = mmap_anon(&addr);
	if (error != 0)
		exit(1);

	wait = NOWAIT;
	oid = SLS_DEFAULT_PARTITION;
	while ((opt = getopt_long(argc, argv, "mpw", memsnap_longopts, NULL)) !=
	    -1) {
		switch (opt) {
		case 'm':
			oid = SLS_DEFAULT_MPARTITION;
			break;

		case 'p':
			wait = BUSYWAIT;
			break;

		case 'w':
			wait = BLOCK;
			break;

		default:
			printf("Usage:./memsnap [-m] [-w]\n");
			exit(1);
			break;
		}
	}

	/* Attach ourselves to the partition. */
	error = sls_attach(oid, getpid());
	if (error != 0)
		exit(1);

	/* Fill the memory region with a char. */
	memset(addr, 'a', MMAP_SIZE);

	/* Do a full checkpoint. */
	error = sls_checkpoint_epoch(oid, false, &nextepoch);
	if (error != 0)
		exit(1);

	/*
	 * If the map is filled with the character we filled it with _later_ in
	 * the code, we have been restored properly and exit with success.
	 */
	if (mmap_isfilled(addr, 'f')) {
		printf("Secret ending!\n");
		exit(0);
	}

	/*
	 * We can only wait for the SLS to flush if we are not a restored
	 * process. We know that because the check above failed.
	 */
	wait_for_sls(nextepoch, oid, wait);

	if (!mmap_isfilled(addr, 'a')) {
		printf("Memory corruption: %.16s\n", (char *)addr);
		return (1);
	}

	/* Snapshot and return an error. */
	for (char c = 'b'; c <= 'f'; c++) {
		/* Fill the memory region with a different char. */
		memset(addr, c, MMAP_SIZE);

		error = sls_memsnap_epoch(oid, addr, &nextepoch);
		if (error != 0)
			exit(1);

		wait_for_sls(nextepoch, oid, wait);
	}
	printf("Regular ending\n");

	exit(1);
}
