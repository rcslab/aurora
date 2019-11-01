
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
#include <slos.h>

#include "../../slos/slos_bnode.h"

static int fd;
static uint32_t bsize = 0;
static uint32_t ssize = 0;
static uint64_t size = 0;
struct slos_sb *sb;
struct bnode *broot;

#define SBBLK	    0
#define BROOTBLK    1
#define SZROOTBLK   2
#define BKTBLK	    3

/* Used in init_bnode below. */
#define max(a, b) (((a) > (b)) ? (a) : (b))

/* Userspace variant of bnode_alloc(). */
static void
init_bnode(struct bnode *bnode, uint64_t blkno, uint64_t vsize, int external)
{

	bnode->blkno = blkno;
	bnode->external = external;
	bnode->vsize = vsize;
	/* 
	 * The spacing between consecutive elements of the array needs
	 * to be able to hold a disk pointer, since the values of
	 * internal bnodes need to be disk pointers themselves.
	 */
	bnode->bsize = ((sb->sb_bsize - sizeof(struct bnode)) / 
	     (max(vsize, sizeof(struct slos_diskptr)) + sizeof(uint64_t))) - 1;
	bnode->size = 0;
	bnode->parent = (struct slos_diskptr) { blkno, 1, 0 };
	bnode->magic = SLOS_BMAGIC;

}

/* Userspace variants of bnode_put{key, val, ptr}(). */
static void
init_bkey(struct bnode *bnode, size_t offset, uint64_t bkey)
{
	uint64_t *keys;

	keys = (uint64_t *) bnode->data;
	keys[offset] = bkey;
}

static void
init_bptr(struct bnode *bnode, size_t offset, struct slos_diskptr diskptr)
{
	struct slos_diskptr *ptrs;

	/* Find the beginning of the values array. */
	ptrs = (struct slos_diskptr *) &bnode->data[(bnode->bsize + 1) * SLOS_KEYSIZE];
	ptrs[offset] = diskptr;
}

static void
init_bval(struct bnode *bnode, size_t offset, void *val)
{
	uint8_t *values;

	/* Find the beginning of the values array. */
	values = &bnode->data[(bnode->bsize + 1) * SLOS_KEYSIZE];

	/* Copy the bytes of the struct out. */
	memcpy(&values[offset * bnode->vsize], val, bnode->vsize);
}

int
write_sb()
{
	uint32_t status;
	uint64_t value;
	int barrsize;
	sb = (struct slos_sb *)calloc(1, bsize);

	sb->sb_magic = SLOS_MAGIC;
	sb->sb_majver = SLOS_MAJOR_VERSION;
	sb->sb_minver = SLOS_MINOR_VERSION;

	uuid_create(&sb->sb_uuid, &status);
	if (status != uuid_s_ok) {
		fprintf(stderr, "Failed to generate a UUID\n");
		return -1;
	}

	sb->sb_ssize = ssize;
	sb->sb_bsize = bsize;
	sb->sb_asize = bsize;
	sb->sb_size = size / bsize;

	/*
	 * The first block is the superblock. 
	 * Then comes the the boot allocator,
	 * where we get blocks for the main allocator btrees. 
	 * The rest of the disk is used for the data itself.
	 *
	 * The root of the offset btree is in the boot allocator. 
	 * We trivially use the first block for the initial
	 * value.
	 */
	sb->sb_broot.offset = BROOTBLK;
	sb->sb_broot.size = 1;

	/* 
	 * Similarly, the root of the 
	 * allocator sizes btree is 
	 * assigned statically.
	 */
	sb->sb_szroot.offset = SZROOTBLK;
	sb->sb_szroot.size = 1;

	/*
	 * Reserve 1% of blocks for the boot allocator. (Note: We assume
	 * the boot allocator area is contiguous, so the superblock can't 
	 * be in the middle of it).
	 */
	sb->sb_bootalloc.offset = BROOTBLK < SZROOTBLK ? BROOTBLK : SZROOTBLK;
	sb->sb_bootalloc.size = sb->sb_size / 100;

	/* 
	 * The number of free blocks for the data is that of the whole disk
	 * minus the superblock and the boot allocator.
	 */
	sb->sb_data.offset = sb->sb_bootalloc.offset + sb->sb_bootalloc.size;
	sb->sb_data.size = sb->sb_size - sb->sb_data.offset;

	/*
	 * The first block in the data region is reserved for the root node
	 * of a btree holding all the inodes in the system.
	 */
	sb->sb_inodes.offset = sb->sb_data.offset;
	sb->sb_inodes.size = 1;

	status = pwrite(fd, sb, bsize, 0);
	if (status < 0) {
		perror("pwritesb");
		return -1;
	}
	

	/* 
	 * Create the root of the alllocator offset btree. 
	 * At the time the OSD is initialized, it's
	 * an external node of size one whose parent
	 * is itself.
	 */
	broot = (struct bnode *) calloc(1, bsize);
	/* The offset tree simply stores sizes. */
	init_bnode(broot, sb->sb_broot.offset, sizeof(uint64_t), BNODE_EXTERNAL);

	/* 
	 * The free region is the data region minus the
	 * first block, reserved for the inode root.
	 */

	init_bkey(broot, 0, sb->sb_data.offset + 1);
	value = sb->sb_data.size - 1;
	init_bval(broot, 0, &value);
	broot->size = 1;

	status = pwrite(fd, broot, bsize, broot->blkno * bsize);
	if (status < 0) {
		perror("pwritebroot");
		free(broot);
		free(sb);
		return -1;
	}

	/* 
	 * Reuse the broot to write to disk the root of 
	 * the allocator size btree. This btree's only
	 * element is a block that holds the offset of
	 * the data region. The key to that block is the
	 * size that region's size.
	 */
	init_bnode(broot, sb->sb_szroot.offset, 
		sizeof(struct slos_diskptr), BNODE_EXTERNAL);
	/* 
	 * The values of the sizes btree are
	 * block pointers to buckets. 
	 */
	init_bkey(broot, 0, sb->sb_data.size - 1);
	init_bval(broot, 0, &DISKPTR_BLOCK(BKTBLK));
	broot->size = 1;

	status = pwrite(fd, broot, bsize, broot->blkno * bsize);
	if (status < 0) {
		perror("pwriteszroot");
		free(broot);
		free(sb);
		return -1;
	}

	/* A size bucket, also represented by a btree. */

	init_bnode(broot, BKTBLK, sizeof(uint64_t), BNODE_EXTERNAL);
	/* 
	 * External bnode values here correspond
	 * to extent offsets. They are the same 
	 * as their keys.
	 */
	init_bkey(broot, 0, sb->sb_data.offset + 1);
	value = sb->sb_data.offset + 1;
	init_bval(broot, 0, &value);
	broot->size = 1;

	status = pwrite(fd, broot, bsize, broot->blkno * bsize);
	if (status < 0) {
		perror("pwritebkt");
		free(broot);
		free(sb);
		return -1;
	}

	/* The root of the inode btree. */
	init_bnode(broot, sb->sb_inodes.offset, 
		sizeof(struct slos_diskptr), BNODE_EXTERNAL);
	/* No elements yet. */
	broot->size = 0;

	status = pwrite(fd, broot, bsize, broot->blkno * bsize);
	if (status < 0) {
		perror("pwriteino");
		free(broot);
		free(sb);
		return -1;
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

		ssize = sectorsize;

		if (bsize == 0) {
			bsize = ssize;
		}

		if (!size || size < disksize) {
			size = disksize;
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

	printf("Disk Size %luGiB\n", size / (1024*1024*1024));
	printf("Block Size %ukiB\n", bsize / 1024);
	printf("Sector Size %uB\n", ssize);

	if (write_sb() < 0)
		exit(1);

	return 0;
}

