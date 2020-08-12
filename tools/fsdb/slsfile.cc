#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "slsfile.h"

#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <vector>

#define ROUNDUP(a, b) ( (((a) + (b - 1)) / (b)) * (b))

std::string
Snapshot::toString(int verbose = 0)
{
	std::stringstream ss;
	ss << "Snapshot " << super.sb_index << " - Epoch " << super.sb_epoch << std::endl;
	if (verbose) {
		ss << "Inode Tree: " << super.sb_root.offset << std::endl;
		ss << "Dirty Meta Synced: " << super.sb_meta_synced << std::endl;
		ss << "Dirty Data Synced: " << super.sb_data_synced << std::endl;
		ss << "Attempted Checkpoints: " << super.sb_attempted_checkpoints << std::endl;
		ss << "Checksum Tree: " << super.sb_cksumtree.offset << std::endl;
		ss << "Allocator Size Tree: " << super.sb_allocsize.offset << std::endl;
		ss << "Allocator Offset Tree: " << super.sb_allocoffset.offset << std::endl;
		ss << "Time(s): " << super.sb_time << std::endl;
		ss << "Time(nsec): " << super.sb_time_nsec << std::endl;
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
		std::cout << "CORRUPTED INO" << std::endl;
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

std::shared_ptr<SFile>
InodeFile::getFile(uint64_t pid)
{
	for (auto k : availableInodes()) {
		if (k.first == pid) {
			return createFile(snap, k.second.offset);
		}
	}

	return nullptr;
}

SReg::SReg(Snapshot *sb, slos_inode &i) : SFile(sb, i, SType::SREG)
{
	tree = Btree<uint64_t, diskptr_t>(snap, ino.ino_btree.offset);
}

SDir::SDir(Snapshot *sb, slos_inode &i) : SFile(sb, i, SType::SDIR)
{
	tree = Btree<uint64_t, diskptr_t>(snap, ino.ino_btree.offset);
}

std::ostream&
SDir::out(std::ostream &where)
{
	auto iter = tree.keymax(0);
	size_t file_off = iter.key() * snap->super.sb_bsize;
	size_t past_off = file_off;
	char zeroes[512] =  {};
	while (iter.valid()) {
		/*
		 * Have to keep in mind about holes in the file
		 */
		file_off = iter.key() * snap->super.sb_bsize;
		size_t offset = iter.val().offset * snap->super.sb_bsize;
		size_t size = std::min(ino.ino_size - file_off, iter.val().size);
		/*
		 * Since the file desscriptor is a device, we have a minimum 
		 * block size we have to maintain which is why for small reads 
		 * and writes we need to bump up the buffer
		 */
		size_t bufsize = ROUNDUP(MAX(size, snap->super.sb_ssize), snap->super.sb_ssize);
		void * buf = malloc(bufsize);
		int readin = pread(snap->dev, buf, bufsize, offset);
		if (readin != bufsize) {
			std::cout << strerror(errno) << std::endl;
			std::cout << "Problem reading in off device" << std::endl;
			exit(-1);
		}

		// Write zeroes
		size_t zesize = 0;
		for (size_t x = 0; x < (file_off - past_off); x += 512) {
			zesize = MIN(file_off - (past_off + x), 512);
			where.write(zeroes, size);
		}

		size_t bo = 0;
		while ((bo + sizeof(struct dirent)) < snap->super.sb_bsize) {
			struct dirent *pdir = (struct dirent *)((char *)buf + bo);
			if (pdir->d_reclen == 0) {
				pdir = NULL;
				break;
			}
			where.write(pdir->d_name, pdir->d_namlen);
			where << ": (";
			where << pdir->d_fileno << ", " << pdir->d_type << ")";
			where << std::endl;
			bo += sizeof(struct dirent);
		}

		free(buf);

		past_off = file_off;
		iter = iter.next();
	}

	return where;
}

std::ostream&
InodeFile::out(std::ostream &where)
{
	std::cout << "NOT IMPLEMENTED" << std::endl;
	return where;
}

std::ostream&
SReg::out(std::ostream &where)
{
	auto iter = tree.keymax(0);
	size_t file_off = iter.key() * snap->super.sb_bsize;
	size_t past_off = file_off;
	char zeroes[512] =  {};
	while (iter.valid()) {
		/*
		 * Have to keep in mind about holes in the file
		 */
		file_off = iter.key() * snap->super.sb_bsize;
		size_t offset = iter.val().offset * snap->super.sb_bsize;
		size_t size = std::min(ino.ino_size - file_off, iter.val().size);
		/*
		 * Since the file desscriptor is a device, we have a minimum 
		 * block size we have to maintain which is why for small reads 
		 * and writes we need to bump up the buffer
		 */
		size_t bufsize = ROUNDUP(MAX(size, snap->super.sb_ssize), snap->super.sb_ssize);
		void * buf = malloc(bufsize);
		int readin = pread(snap->dev, buf, bufsize, offset);
		if (readin != bufsize) {
			std::cout << strerror(errno) << std::endl;
			std::cout << "Problem reading in off device" << std::endl;
			exit(-1);
		}

		// Write zeroes
		size_t zesize = 0;
		for (size_t x = 0; x < (file_off - past_off); x += 512) {
			zesize = MIN(file_off - (past_off + x), 512);
			where.write(zeroes, size);
		}

		where.write((char *)buf, size);
		free(buf);

		past_off = file_off;
		iter = iter.next();
	}

	return where;
}

std::ostream& 
operator<<(std::ostream& os, SFile& file)
{
	file.out(os);
	os.flush();
	return os;
}

int
SFile::dumpTo(std::string path)
{
	std::ofstream file; 
	file.open(path);
	file << *this;
	file.close();

	return (0);
}


int
InodeFile::dumpTo(std::string path)
{
	std::cout << "NOT IMPLEMENTED" << std::endl;
	return (-1);
}

std::vector<std::pair<uint64_t, diskptr_t>>
InodeFile::availableInodes()
{
	std::vector<std::pair<uint64_t, diskptr_t>> arr;
	auto root = tree.getRoot();
	auto iter = tree.keymax(0);
	while (iter.valid()) {
		arr.push_back(std::make_pair(iter.key(), iter.val()));
		iter = iter.next();
	}

	return arr;
}

std::string
SFile::toString()
{
	std::stringstream ss;
	ss << "Inode " << ino.ino_pid << std::endl;
	ss << snap->toString();
	if (S_ISDIR(ino.ino_mode)) {
		ss << "Type: VDIR" << std::endl;
	} else if (S_ISBLK(ino.ino_mode)) {
		ss << "Type: VBLK" << std::endl;
	} else if (S_ISCHR(ino.ino_mode)) {
		ss << "Type: VCHR" << std::endl;
	} else if (S_ISREG(ino.ino_mode)) {
		ss << "Type: VREG" << std::endl;
	}
	ss << "Btree Root " << ino.ino_btree.offset << std::endl;
	ss << "aSize: " << ino.ino_asize << std:: endl;
	ss << "Size: " << ino.ino_size << std:: endl;
	ss << "Links: " << ino.ino_nlink << std:: endl;

	return ss.str();
}

int 
SFile::failed() {
	if (snap == nullptr || error == (-1)) {
		std::cout << "Un-inited file" << std::endl;
		return (-1);
	} 
	
	return (error);
}

InodeFile::InodeFile(Snapshot *sb, slos_inode &i) : SFile(sb, i, SType::INODE_ROOT) {
	tree = Btree<uint64_t, diskptr_t>(snap, ino.ino_btree.offset);
};
