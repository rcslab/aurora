
#include <sys/types.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <slos.h>
#include <slsfs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <uuid.h>

int
main(int argc, const char *argv[])
{
	int status;
	struct stat st;
	int fd;
	uint32_t bsize = 0;
	uint32_t ssize = 0;
	uint64_t size = 0;

	if (argc != 2) {
		printf("Usage: %s DEVICE\n", argv[0]);
		exit(1);
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	status = fstat(fd, &st);
	if (status < 0) {
		perror("lstat");
		exit(1);
	}

	if (!bsize || bsize < st.st_blksize) {
		bsize = st.st_blksize;
	}

	if (S_ISCHR(st.st_mode)) {
		int sectorsize;
		off_t disksize;

		if (ioctl(fd, DIOCGSECTORSIZE, &sectorsize) < 0) {
			perror("ioctl(DIOCGSECTORSIZE)");
			exit(1);
		}

		if (ioctl(fd, DIOCGMEDIASIZE, &disksize) < 0) {
			perror("ioctl(DIOGCGMEDIASIZE)");
			exit(1);
		}

		ssize = sectorsize;
		size = disksize;

		if (bsize == 0) {
			bsize = ssize;
		}
	} else if (S_ISREG(st.st_mode)) {
		if (!size || size < st.st_size) {
			size = st.st_size;
		}

		/*
		 * Set the sector size to 4 KiB and block size to the maximum
		 * block size.
		 */
		ssize = 4 * 1024;
		bsize = 64 * 1024;
	} else {
		fprintf(
		    stderr, "You can only create an OSD on a device or file\n");
		exit(1);
	}

	printf(
	    "%s: %lu GiB (%lu sectors), block size %u kiB, sector size %u B\n",
	    argv[1], size / (1024 * 1024 * 1024), size / ssize, bsize / 1024,
	    ssize);

	// We have to allocate the appropriate super blocks

	static_assert(sizeof(struct slos_sb) <= 512,
	    "superblock larger than sector size");

	printf("creating super blocks\n");
	struct slos_sb *sb = (struct slos_sb *)malloc(ssize);
	memset(sb, 0, ssize);
	sb->sb_magic = SLOS_MAGIC;
	sb->sb_epoch = EPOCH_INVAL;
	sb->sb_ssize = ssize;
	sb->sb_bsize = bsize;
	sb->sb_size = size;
	sb->sb_asize = bsize;
	sb->sb_used = 0;
	sb->sb_sas_addr = SLS_SAS_INITADDR;

	for (int i = 0; i < NUMSBS; i++) {
		sb->sb_index = i;
		ssize_t written = write(fd, sb, ssize);
		if (written == (-1)) {
			perror("writing superblock failed");
			free(sb);
			return (1);
		}
	}
	free(sb);

	return (0);
}
