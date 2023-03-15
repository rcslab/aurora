#include <sys/stat.h>
#include <sys/sysproto.h>

#include <assert.h>
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

int
main()
{
	char readme[256];
	memset(readme, '\0', 256);
	char mywrite[MB];
	int rand = open("/dev/random", O_RDONLY);
	int written;
	int total_write = 0;

	int wal_fd = slsfs_create_wal("test_wal.log", O_RDWR, 0600, 1 * GB);
	if (wal_fd < 0) {
		return -1;
	}

	written = read(rand, mywrite, MB);
	if (written != MB) {
		perror("Problem reading random");
		return errno;
	}

	for (int i = 0; i < 1024; i++) {
		written = write(wal_fd, mywrite, MB);
		if (written != MB) {
			perror("Problem writing to the WAL");
			return errno;
		}
		total_write += MB;
	}
	written = write(wal_fd, mywrite, MB);
	if (errno != ENOMEM) {
		printf("Should be an error\n");
		return -1;
	}

	if (total_write != GB) {
		printf("Not enough written");
		return -1;
	}

	fsync(wal_fd);

	written = pread(wal_fd, readme, 256, 0);
	if (written < 0) {
		perror("Problem reading from the WAL");
		return errno;
	}

	close(wal_fd);

	if (strncmp(readme, mywrite, 10) != 0) {
		printf("What was written to the WAL was not what was read\n");
		return -1;
	}

	int reg_fd = open("test_wal.log", O_RDWR);
	if (reg_fd < 0) {
		perror("Could not open file");
		return -1;
	}

	written = read(reg_fd, readme, 256);
	if (written < 0) {
		perror("Did not properly read back WAL in regular open path");
		return errno;
	}

	if (strncmp(readme, mywrite, 10) != 0) {
		perror(
		    "What was written to the W#AL was not what was read in regular open");
		return -1;
	}
}
