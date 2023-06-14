#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <slos.h>
#include <sls.h>
#include <slsfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KB (1024)
#define MB (1024 * KB)
#define GB (1024 * MB)

#define SIZE (10 * MB)

#define MSG ("Scrawling all over the SAS mapping")

#define PATH ("/sasfile")
/*
 * CAREFUL: this needs to be a divisor of 4096 (PAGE_SIZE)
 * and larger than strnlen(MSG) for the test to work.
 */
#define ITERS (128)

void
use_file(char *name)
{
	size_t off = SIZE / ITERS;
	const size_t size = SIZE;
	char fullpath[PATH_MAX];
	char buf[sizeof(MSG)];
	char *oldsas, *sas, *addr;
	ssize_t ret;
	int error;
	int fd;
	int i;

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

	memset(fullpath, 0, PATH_MAX);
	memcpy(fullpath, name, strnlen(name, PATH_MAX));

	/* Create the first mapping. */
	error = slsfs_sas_map(fd, (void **)&sas);
	if (error != 0) {
		printf("slsfs_sas_map failed\n");
		exit(1);
	}

	/* Dirty the address space. */
	for (i = 0; i < ITERS; i++) {
		addr = &sas[off * i];
		strncpy(addr, MSG, sizeof(MSG));
	}

	oldsas = sas;
	/* Remove the mapping and the fd. */
	munmap(sas, MAX_SAS_SIZE);
	close(fd);

	/*
	 * Reopen the file and reset the mapping,
	 * ensure it is at the same address.
	 */
	fd = open(name, O_RDWR, 0666);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	error = slsfs_sas_map(fd, (void **)&sas);
	if (error != 0) {
		printf("slsfs_sas_map failed\n");
		exit(1);
	}

	if (sas != oldsas) {
		printf(
		    "Inconsistent mapping address, %p then %p\n", oldsas, sas);
		exit(1);
	}

	/* Dirty the address space. */
	for (i = 0; i < ITERS; i++) {
		addr = &sas[off * i];
		strncpy(buf, addr, sizeof(MSG));
		if (strncmp(addr, MSG, sizeof(MSG)) != 0) {
			printf("Improper read (%s) (%s)\n", addr, MSG);
			exit(1);
		}
	}
}

int
main(int argc, char *argv[])
{
	char fullname[PATH_MAX];
	char fileid[PATH_MAX];
	size_t numfiles;
	int i;

	if (argc < 3) {
		printf("Usage: ./sas <path> <# of files>\n");
		exit(1);
	}

	numfiles = strtol(argv[2], NULL, 10);
	if (numfiles == 0)
		exit(1);

	for (i = 0; i < numfiles; i++) {
		snprintf(fileid, PATH_MAX, "%06d", i);

		memset(fullname, 0, PATH_MAX);
		strncpy(fullname, argv[1], strnlen(argv[1], PATH_MAX));
		strncat(fullname, PATH, strnlen(PATH, PATH_MAX));
		strncat(fullname, fileid, strnlen(fileid, PATH_MAX));
		printf("Full name: %s\n", fullname);
		use_file(fullname);
		printf("Iteration %d done\n", i);
	}

	return (0);
}
