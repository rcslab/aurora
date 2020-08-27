
#include <sys/stat.h>

#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <uuid.h>

#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <vector>

#include <slos.h>
#include <slos_inode.h>

#include "snapshot.h"
#include "btree.h"
#include "file.h"
#include "directory.h"
#include "util.h"

InodeFile::InodeFile(Snapshot *sb, slos_inode &i)
    : SFile(sb, i, SType::INODE_ROOT)
{
};

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

SReg::SReg(Snapshot *sb, slos_inode &i)
    : SFile(sb, i, SType::SREG)
{
}


SFile::SFile()
    : snap(nullptr)
{
}

SFile::SFile(Snapshot *snap, slos_inode &ino, SType type)
    : snap(snap), ino(ino), type(type)
{
	tree = Btree<uint64_t, diskptr_t>(snap, ino.ino_btree.offset);
}
std::string
rec_to_string(uint64_t type)
{
    switch (type) {
	case SLOSREC_INVALID:
		return "Invalid";
	case SLOSREC_PROC:
		return "Process Info";
	case SLOSREC_SESS:
		return "Session Info";
	case SLOSREC_MEM:
		return "VM Space";
	case SLOSREC_VMOBJ:
		return "VM Object";
	case SLOSREC_FILE:
		return "File";
	case SLOSREC_SYSVSHM:
		return "SysV Shared Memory";
	case SLOSREC_SOCKBUF:
		return "Sockbuf";
	case SLOSREC_DIR:
		return "Directory";
	case SLOSREC_DATA:
		return "Data";
	case SLOSREC_MANIFEST:
		return "Manifest";
	default:
		return "Unknown Type";
    }
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
	ss << "Inode #/PID: " << ino.ino_pid << std::endl;
	ss << "UID: " << ino.ino_uid << std::endl;
	ss << "GID: " << ino.ino_gid << std::endl;
	ss << "Creation Time: " << time_to_string(ino.ino_ctime, ino.ino_ctime_nsec) << std::endl;
	ss << "Modification Time: " << time_to_string(ino.ino_mtime, ino.ino_mtime_nsec) << std::endl;
	ss << "Access Time: " << time_to_string(ino.ino_atime, ino.ino_atime_nsec) << std::endl;
	ss << "Birthday Time: " << time_to_string(ino.ino_birthtime, ino.ino_birthtime_nsec) << std::endl;
	ss << "Record Type: " << rec_to_string(ino.ino_rstat.type) << std::endl;
	ss << "Record Length: " << ino.ino_rstat.len << std::endl;

	return ss.str();
}

int 
SFile::failed() {
	if (snap == nullptr || error == (-1)) {
		std::cout << "Uninitialized file" << std::endl;
		return (-1);
	} 
	
	return (error);
}

void
SFile::writeData(std::ostream &where)
{
	auto iter = tree.keymax(0);
	size_t file_off = iter.key() * snap->super.sb_bsize;
	size_t past_off = file_off;
	char zeroes[SECTOR_SIZE] =  {};

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
		for (size_t x = 0; x < (file_off - past_off); x += SECTOR_SIZE) {
			zesize = MIN(file_off - (past_off + x), SECTOR_SIZE);
			where.write(zeroes, size);
		}

		where.write((char *)buf, size);
		free(buf);

		past_off = file_off;
		iter = iter.next();
	}

	return;
}

void
SFile::print()
{
	writeData(std::cout);
	return;
}

void
SFile::hexdump()
{
	auto iter = tree.keymax(0);
	size_t file_off = iter.key() * snap->super.sb_bsize;
	size_t past_off = file_off;
	char zeroes[512] =  {};

	if (ino.ino_size == 0) {
		printf("Empty file\n");
		return;
	}

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
			::hexdump(zeroes, 512, past_off + x);
		}

		::hexdump((char *)buf, size, file_off);
		free(buf);

		past_off = file_off;
		iter = iter.next();
	}

	return;
}

int
SFile::dumpTo(std::string path)
{
	std::ofstream file; 
	file.open(path);
	writeData(file);
	file.close();

	return (0);
}

