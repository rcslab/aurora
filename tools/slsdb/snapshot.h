#ifndef __SNAPSHOT_H__
#define __SNAPSHOT_H__

class SFile;
class InodeFile;

class Snapshot {
    public:
	Snapshot(int dev, struct slos_sb &sb)
	    : super(sb)
	    , dev(dev) {};
	std::string toString(int verbose = 0);
	std::shared_ptr<InodeFile> getInodeFile();

	struct slos_sb super;
	int dev;
};

std::shared_ptr<SFile> createFile(Snapshot *sb, long blknum);

#endif
