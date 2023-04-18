#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <slos.h>
#include <slos_inode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sysexits.h>
#include <unistd.h>

#include "radixdbg.h"
#include "radixnode.h"

struct slos_sb superblocks[NUMSBS];
char buf[SECTORSIZE];

void
read_superblocks(int fd, struct slos_sb **headsbp)
{
	struct slos_sb *headsb = NULL;
	int ret;
	int i;

	/* Find the superblocks, find the most recent one */
	for (i = 0; i < NUMSBS; i++) {
		ret = pread(fd, buf, SECTORSIZE, SECTORSIZE * i);
		if (ret < 0) {
			perror("read");
			exit(EX_DATAERR);
		}

		if (ret != SECTORSIZE) {
			printf(
			    "Error: Read %d, expected %d\n", ret, SECTORSIZE);
			exit(EX_DATAERR);
		}

		memcpy(&superblocks[i], buf, sizeof(superblocks[i]));

		if (SECTORSIZE != superblocks[i].sb_ssize) {
			printf("Expected sector size %d, got %ld\n", SECTORSIZE,
			    superblocks[i].sb_ssize);
		}
		if (BLKSIZE != superblocks[i].sb_bsize) {
			printf("Expected block size %d, got %ld\n", SECTORSIZE,
			    superblocks[i].sb_bsize);
		}
	}

	/* Integrity check the superblocks. */
	for (i = 0; i < NUMSBS; i++) {
		if (superblocks[i].sb_magic != SLOS_MAGIC) {
			printf(
			    "Superblock magic for index %d is %lx, expected %lx\n",
			    i, superblocks[i].sb_magic, SLOS_MAGIC);
			exit(EX_DATAERR);
		}

		if (headsb == NULL ||
		    (superblocks[i].sb_epoch != EPOCH_INVAL &&
			superblocks[i].sb_epoch > headsb->sb_epoch)) {
			headsb = &superblocks[i];
		}
	}

	*headsbp = headsb;
}

void
read_inode_single(int fd, uint64_t lblkno, struct slos_inode *inop)
{
	struct slos_inode *inobuf;
	int ret;

	ret = pread(fd, buf, BLKSIZE, BLKSIZE * lblkno);
	if (ret < 0) {
		perror("read");
		exit(EX_DATAERR);
	}
	assert(ret == BLKSIZE);

	inobuf = (struct slos_inode *)buf;
	*inop = *inobuf;
}

void
read_inodes(int fd, struct slos_sb *sb)
{
	struct stree_iter siter;
	struct slos_inode ino;
	int error;

	assert(sb->sb_magic == SLOS_MAGIC);
	assert(sb->sb_epoch != EPOCH_INVAL);

	read_inode_single(fd, sb->sb_root.offset, &ino);
	assert(ino.ino_magic == SLOS_IMAGIC);

	error = siter_start(fd, ino.ino_btree.offset, 0, &siter);
	assert(error == 0);

	do {
		read_inode_single(fd, siter.siter_value.offset, &ino);
		if (ino.ino_pid != siter.siter_key) {
			printf(
			    "Disparity: PID %lx vs Offset %lx (Physical offset %lx)\n",
			    ino.ino_pid, siter.siter_key,
			    siter.siter_value.offset);
		}

		if (ino.ino_magic != SLOS_IMAGIC) {
			printf("inode at offset %lx has magic number %lx\n",
			    siter.siter_key, ino.ino_pid);
			exit(EX_OSERR);
		}
	} while (siter_iter(fd, &siter) == 0);

	/* XXX Add basic radix tree checksumming and sanity checking. */
	printf("Inodes traversed\n");
}

int
main(int argc, char **argv)
{
	struct slos_sb *headsb;
	char *diskpath;
	int fd;

	/* Open the disk. */
	diskpath = argv[1];
	fd = open(diskpath, O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(EX_USAGE);
	}

	read_superblocks(fd, &headsb);
	printf("Superblocks successful\n");

	/* Read the inodes, see if they are consistent */
	read_inodes(fd, headsb);

	/* Read the trees for each inode */

	/* Read the actual data and do checksumming. */

	return (0);
}
