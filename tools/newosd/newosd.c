
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/disk.h>

#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid.h>

#include <sls.h>
#include <sls_osd.h>

static int fd;
static uint32_t bsize = 0;
static uint64_t size = 0;
struct slsosd *sb;

int
write_sb()
{
	int status;
	sb = (struct slsosd *)calloc(1, bsize);

	sb->osd_magic = SLSOSD_MAGIC;
	sb->osd_majver = SLSOSD_MAJOR_VERSION;
	sb->osd_minver = SLSOSD_MINOR_VERSION;

	uuid_create(&sb->osd_uuid, &status);
	if (status != uuid_s_ok) {
		fprintf(stderr, "Failed to generate a UUID\n");
		return -1;
	}

	sb->osd_bsize = bsize;
	sb->osd_asize = bsize;
	sb->osd_size = size / bsize;

	/*
	 * Reserve 0.1% of blocks for inodes
	 */
	sb->osd_numinodes = sb->osd_size / 1000;
	/*
	 * Number of available blocks is the total number of blocks
	 * minus the blocks reserved for superblock and free bitmap.
	 */
	sb->osd_numblks = sb->osd_size - 1;
	/*
	 * First block is the superblock, second is the allocation bitmap.  
	 * Allocation bitmap contains one byte per block and contains a byte
	 * for every block from the beginning of the volume (e.g. including the 
	 * superblock, free bitmap, and inode table).  The free bitmap is used 
	 * to manage allocation of both the inodes and the disk blocks.  The 
	 * file system needs to start at the correct offset on the disk.
	 */
	sb->osd_allocoff.ptr_offset = 1;
	sb->osd_inodeoff.ptr_offset = 2 + ((sb->osd_size + bsize - 1)/bsize);
	sb->osd_firstblk.ptr_offset = sb->osd_inodeoff.ptr_offset + sb->osd_numinodes;
	// Adjust the number of free blocks;
	sb->osd_numblks = sb->osd_size - sb->osd_firstblk.ptr_offset;

	status = pwrite(fd, sb, bsize, 0);
	if (status < 0) {
		perror("pwritesb");
		return -1;
	}

	return 0;
}

int
write_bitmap()
{
	int status;
	char zeroblk[bsize];
	uint64_t bmapsize = ((sb->osd_size + bsize - 1)/bsize);

	memset(&zeroblk, 0, bsize);

	for (uint64_t i = 0; i < bmapsize; i++) {
		status = pwrite(fd, &zeroblk, bsize,
		    (sb->osd_allocoff.ptr_offset + i)*bsize);
		if (status < 0) {
			perror("pwrite");
			return -1;
		}
	}

	return 0;
}

int
main(int argc, const char *argv[])
{
	int status;
	struct stat st;

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

		if (bsize == 0) {
			bsize = sectorsize;
		}
		if (!size || size < disksize) {
			size = disksize;
		}
	} else if (S_ISREG(st.st_mode)) {
		if (!size || size < st.st_size) {
			size = st.st_size;
		}
	} else {
		fprintf(stderr, "You can only create an OSD on a device or file\n");
		exit(1);
	}

	printf("Disk Size %luGiB\n", size / (1024*1024*1024));
	printf("Block Size %ukiB\n", bsize / 1024);

	if (write_sb() < 0)
		exit(1);

	if (write_bitmap() < 0)
		exit(1);

	return 0;
}

