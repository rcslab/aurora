#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/disk.h>
#include <sys/vnode.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h> 
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <uuid.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

#include <slos.h>
#include <slsfs.h>

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

		printf("Size of disk %lu\n", disksize);
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
		 * Have the block size be equal to the standard 
		 * system maximum block size.
		 */
		bsize = 64 * 1024;
	} else {
		fprintf(stderr, "You can only create an OSD on a device or file\n");
		exit(1);
	}

	printf("%lu size\n", size);
	printf("Disk Size %luGiB\n", size / (1024*1024*1024));
	printf("Block Size %ukiB\n", bsize / 1024);
	printf("Sector Size %uB\n", ssize);

	// We have to allocate the appropriate super blocks
	
	struct slos_sb * sb = (struct slos_sb *)malloc(ssize);
	printf("Size of superblock %lu\n", sizeof(struct slos_sb));
	printf("%llu magic\n", SLOS_MAGIC);
	sb->sb_magic = SLOS_MAGIC;
	sb->sb_epoch = EPOCH_INVAL;
	sb->sb_ssize = ssize;
	sb->sb_bsize = bsize;
	sb->sb_size = size;
	sb->sb_asize = bsize;
	for (int i = 0; i < NUMSBS; i++) {
		sb->sb_index = i;
		ssize_t written = write(fd, sb, ssize);
		if (written == (-1)) {
			printf("Problem writing");
			printf("%s\n", strerror(errno));
		}
	}

	return (0);
}

