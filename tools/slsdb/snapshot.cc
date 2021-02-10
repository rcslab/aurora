
#include <sys/stat.h>

#include <dirent.h>
#include <fcntl.h>
#include <slos.h>
#include <slos_btree.h>
#include <slos_inode.h>
#include <unistd.h>
#include <uuid.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "btree.h"
#include "directory.h"
#include "file.h"
#include "snapshot.h"
#include "util.h"

std::string
Snapshot::toString(int verbose)
{
	std::stringstream ss;
	char *uuidstr;

	ss << "Snapshot " << super.sb_index << " - Epoch " << super.sb_epoch
	   << std::endl;
	if (verbose) {
		ss << "Version: " << super.sb_majver << "." << super.sb_minver
		   << std::endl;
		ss << "Features: 0x" << std::setw(8) << std::setfill('0')
		   << std::setbase(16) << super.sb_flags << std::endl;
		uuid_to_string(&super.sb_uuid, &uuidstr, NULL);
		ss << "UUID: " << uuidstr << std::endl;
		ss << "Volume Name: " << super.sb_name << std::endl;
		ss << "Sector Size: " << super.sb_ssize << std::endl;
		ss << "Block Size: " << super.sb_bsize << std::endl;
		ss << "Allocation Size: " << super.sb_asize << std::endl;
		ss << "Size: " << super.sb_size << std::endl;
		ss << "Inode Tree: " << super.sb_root.offset << std::endl;
		ss << "Attempted Checkpoints: "
		   << super.sb_attempted_checkpoints << std::endl;
		ss << "Checksum Tree: " << super.sb_cksumtree.offset
		   << std::endl;
		ss << "Allocator Size Tree: " << super.sb_allocsize.offset
		   << std::endl;
		ss << "Allocator Offset Tree: " << super.sb_allocoffset.offset
		   << std::endl;
		ss << "Time: "
		   << time_to_string(super.sb_time, super.sb_time_nsec)
		   << std::endl;
		ss << "Last Mounted Time: " << time_to_string(super.sb_mtime, 0)
		   << std::endl;
		ss << "Dirty Meta Synced: " << super.sb_meta_synced
		   << std::endl;
		ss << "Dirty Data Synced: " << super.sb_data_synced
		   << std::endl;
	}

	return ss.str();
}

std::shared_ptr<SFile>
createFile(Snapshot *sb, long blknum)
{
	struct slos_inode ino;
	size_t blksize = sb->super.sb_bsize;
	char buf[sb->super.sb_bsize];
	int readin = pread(sb->dev, buf, blksize, blknum * blksize);
	if (readin != blksize) {
		return nullptr;
	}

	memcpy(&ino, buf, sizeof(struct slos_inode));

	if (ino.ino_magic != SLOS_IMAGIC) {
		std::cout << "Inode magic mismatch" << std::endl;
		return nullptr;
	}

	if (S_ISDIR(ino.ino_mode)) {
		return std::make_shared<SDir>(sb, ino);
	} else if (S_ISBLK(ino.ino_mode)) {
		std::cout << "Not implemented" << std::endl;
		return nullptr;
	} else if (S_ISCHR(ino.ino_mode)) {
		std::cout << "Not implemented" << std::endl;
		return nullptr;
	} else if (S_ISREG(ino.ino_mode)) {
		return std::make_shared<SReg>(sb, ino);
	} else if (S_ISFIFO(ino.ino_mode)) {
		std::cout << "Not implemented" << std::endl;
		return nullptr;
	} else if (S_ISLNK(ino.ino_mode)) {
		std::cout << "Not implemented" << std::endl;
		return nullptr;
	} else if (S_ISSOCK(ino.ino_mode)) {
		std::cout << "Not implemented" << std::endl;
		return nullptr;
	} else if (ino.ino_pid == 0) {
		return std::make_shared<InodeFile>(sb, ino);
	}

	return nullptr;
}

std::shared_ptr<InodeFile>
Snapshot::getInodeFile()
{
	auto f = createFile(this, super.sb_root.offset);
	if (f == nullptr) {
		return nullptr;
	}
	return std::dynamic_pointer_cast<InodeFile>(f);
}
