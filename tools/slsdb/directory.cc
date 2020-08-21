
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

#include "btree.h"
#include "snapshot.h"
#include "file.h"
#include "directory.h"
#include "util.h"

SDir::SDir(Snapshot *sb, slos_inode &i) : SFile(sb, i, SType::SDIR)
{
}

std::string
dtype_to_string(uint16_t dtype)
{
	switch (dtype) {
		case DT_UNKNOWN:
			return "Unknown";
		case DT_FIFO:
			return "FIFO";
		case DT_CHR:
			return "Character Device";
		case DT_DIR:
			return "Directory";
		case DT_BLK:
			return "Block Device";
		case DT_REG:
			return "Regular File";
		case DT_LNK:
			return "Symbol Link";
		case DT_SOCK:
			return "Socket";
		default:
			return "Undefined Type";
	}
}

void
SDir::print()
{
	auto iter = tree.keymax(0);
	size_t file_off = iter.key() * snap->super.sb_bsize;
	size_t past_off = file_off;

	while (iter.valid()) {
		/*
		 * Have to keep in mind about holes in the file
		 */
		file_off = iter.key() * snap->super.sb_bsize;
		size_t offset = iter.val().offset * snap->super.sb_bsize;
		size_t size = std::min(ino.ino_size - file_off, iter.val().size);
		/*
		 * Since the file descriptor is a device, we have a minimum 
		 * block size we have to maintain which is why for small reads 
		 * and writes we need to bump up the buffer
		 */
		size_t bufsize = ROUNDUP(MAX(size, snap->super.sb_ssize), snap->super.sb_ssize);
		void * buf = malloc(bufsize);
		int readin = pread(snap->dev, buf, bufsize, offset);
		if (readin != bufsize) {
			std::cout << strerror(errno) << std::endl;
			std::cout << "Problem reading in off device" << std::endl;
			exit(1);
		}

		size_t bo = 0;
		while ((bo + sizeof(struct dirent)) < snap->super.sb_bsize) {
			struct dirent *pdir = (struct dirent *)((char *)buf + bo);
			if (pdir->d_reclen == 0) {
				pdir = NULL;
				break;
			}
			std::cout.write(pdir->d_name, pdir->d_namlen);
			std::cout << ": (" <<  pdir->d_fileno << ", "
			      << dtype_to_string(pdir->d_type) << ")" << std::endl;
			bo += sizeof(struct dirent);
		}

		free(buf);

		past_off = file_off;
		iter = iter.next();
	}

	return;
}

