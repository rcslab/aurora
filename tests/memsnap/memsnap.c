#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

#include <sys/param.h>
#include <sys/mman.h>

#define MMAP_SIZE (PAGE_SIZE * 64)

static int
mmap_anon(void **addr)
{
	void *mapping;

	mapping = mmap(0x100000000, MMAP_SIZE, PROT_READ | PROT_WRITE,
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
		if (((char *) addr)[i] != c) {
			return (false);
		}
	}

	return (true);
}

int
main(int argc, char **argv)
{
	void *addr;
	int error;
	uint64_t oid;

	/* Create the anonymous mapping*/
	error = mmap_anon(&addr);
	if (error != 0)
		exit(1);

	oid = (argc > 1) ? SLS_DEFAULT_MPARTITION : SLS_DEFAULT_PARTITION;

	/* Attach ourselves to the partition. */
	error = sls_attach(oid, getpid());
	if (error != 0)
		exit(1);

	/* Fill the memory region with a char. */
	memset(addr, 'a', MMAP_SIZE);

	/* Do a full checkpoint. */
	error = sls_checkpoint(oid, false);
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

	if (!mmap_isfilled(addr, 'a')) {
		printf("Memory corruption: %.16s\n", (char *)addr);
		return (1);
	}

	/* Snapshot and return an error. */
	for (char c = 'b'; c <= 'f'; c++) {
		/* Fill the memory region with a different char. */
		memset(addr, c, MMAP_SIZE);

		error = sls_memsnap(oid, addr);
		if (error != 0)
			exit(1);
	}
	printf("Regular ending\n");

	exit(1);
}
