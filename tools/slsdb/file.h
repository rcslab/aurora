#ifndef __SLSFILE_H__
#define __SLSFILE_H__ 

class SFile;
class InodeFile;

enum class SType { INODE_ROOT, SREG, SDIR, CKSUM, SBLK, SCHR };

class SFile {
public:
	SFile();
	SFile(Snapshot *snap, slos_inode &ino, SType type);
	virtual ~SFile() {};

	int failed();
	std::string toString();

	int dumpTo(std::string path);
	virtual void print();
	void hexdump();

	friend std::shared_ptr<SFile> createFile(Snapshot *sb, long blknum);
protected:
	void writeData(std::ostream &where);

	struct slos_inode ino;
	long blknum;
	int error;
	SType type;
	Snapshot *snap;

	Btree<size_t, diskptr_t> tree;
};

class SReg : public SFile {
public:
	SReg() : SFile() {};
	SReg(Snapshot *sb, slos_inode &ino);
	~SReg() {};

	void print() {};
};

class InodeFile : public SFile {
public:
	InodeFile(Snapshot *sb, slos_inode &ino);
	~InodeFile() {};

	void print() {};

	std::shared_ptr<SFile> getFile(uint64_t inodeNum);
	std::vector<std::pair<uint64_t, diskptr_t>> availableInodes();
};

#endif
