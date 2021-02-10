#ifndef __DIRECTORY_H__
#define __DIRECTORY_H__

#include "file.h"

class SDir : public SFile {
    public:
	SDir()
	    : SFile() {};
	SDir(Snapshot *sb, slos_inode &ino);
	~SDir() {};

	void print();
};

#endif
