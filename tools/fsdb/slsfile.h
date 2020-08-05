#ifndef __SLSBFILE_H__
#define __SLSBFILE_H__ 

extern "C" {
#include <slos.h>
#include <slsfs.h>
#include <slos_inode.h>
}

#include <string>
#include <vector>

#include "slsbtree.h"

class SFile;
class InodeFile;

class Snapshot {
	public:
		Snapshot(int dev, struct slos_sb &sb) : 
		    super(sb), dev(dev) {};
		std::string toString(int verbose);
		InodeFile *getInodeFile();

		struct slos_sb super;
		int dev;
};

enum class SType { INODE_ROOT, SREG, SDIR, CKSUM, SBLK, SCHR};



class SFile {
	public:
		SFile() : snap(nullptr) {};
		SFile(Snapshot *snap, slos_inode &ino, SType type) : 
		    snap(snap), ino(ino), type(type) {};

		virtual ~SFile() {};

		int failed();
		std::string toString();

		struct slos_inode ino;
		long blknum;
		int error;
		SType type;
		Snapshot *snap;

		virtual int dumpTo(std::string path);
		virtual std::ostream& out(std::ostream &where) = 0;
		

		friend SFile *createFile(Snapshot *sb, long blknum);
		friend std::ostream& operator<<(std::ostream& os, SFile& file);
};

class SReg : public SFile {
	public:
		SReg() : SFile() {};
		SReg(Snapshot *sb, slos_inode &ino);
		~SReg() {};

		Btree<size_t, diskptr_t> tree;
		std::ostream& out(std::ostream &where);

};

class SDir : public SFile {
	public:
		SDir() : SFile() {};
		SDir(Snapshot *sb, slos_inode &ino);
		~SDir() {};

		Btree<size_t, diskptr_t> tree;
		std::ostream& out(std::ostream &where);
};



class InodeFile : public SFile {
	public:
		InodeFile(Snapshot *sb, slos_inode &ino);
		~InodeFile() {};
		SFile * getFile(uint64_t inodeNum);

		Btree<size_t, diskptr_t> tree;
		int dumpTo(std::string path);
		std::ostream& out(std::ostream &where);

		std::vector<std::pair<uint64_t, diskptr_t>> availableInodes();
};
#endif
