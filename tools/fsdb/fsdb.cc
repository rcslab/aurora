#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <histedit.h>

extern "C" {
#include <slsfs.h>
#include <slos.h>
#include <btree.h>
#include <slos_inode.h>
}

#include <iostream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <map>

#include "slsfile.h"
#include "slsbtree.h"

using namespace std;

int dev;
size_t blksize;
size_t sectorsize;
int err = 0;
vector<Snapshot> snaps;
Snapshot * curr = nullptr;
SFile * currinode = nullptr;

#define MAX_ARGC (32)

int 
retrieveSnaps(vector<Snapshot> &snaps)
{
	if (snaps.size() != 0) {
		return (0);
	}
	char buf[512] = {};
	struct slos_sb sb;
	int readin;
	for (int i = 0; i < NUMSBS; i++) {
		off_t offset = i * sectorsize;
		readin = pread(dev, buf, sectorsize, offset);
		if (readin != sectorsize) {
			cout << strerror(errno) << " " << errno << endl;
			return (-1);
		}

		memcpy(&sb, buf, sizeof(struct slos_sb));

		if (sb.sb_magic != SLOS_MAGIC) {
			cout << "Corrupted superblock at " << i << endl;
		}

		if (sb.sb_epoch != EPOCH_INVAL) {
			snaps.push_back(Snapshot { dev, sb });
		} else {
			break;
		}
	}
	return (0);
};

int 
listsnaps(Snapshot *sb, vector<string> &args)
{
	int error;

	error = retrieveSnaps(snaps);
	if (error) {
		return error;
	}

	cout << "Snapshots on Disk" << endl;
	cout << "=================" << endl;
	cout << endl;
	for (auto k : snaps) {
		cout << k.toString(1) << endl;
	}

	return (0);
}


int 
selectsnap(Snapshot *sb, vector<string> &args)
{
	int error;

	if (args.size() != 2) {
		cout << "Bad arguments" << endl;
		return (-1);
	}
	int snap  = strtol(args[1].c_str(), NULL, 10);

	error = retrieveSnaps(snaps);
	if (error) {
		return error;
	}

	if (snap >= snaps.size()) {
		cout << "Bad arguments" << endl;
		return (-1);
	}

	curr = &snaps[snap];

	cout << curr->toString(1) << endl;

	return (0);
}

int
selectinode(Snapshot *sb, vector<string> &args)
{
	if (sb == nullptr) {
		cout << "Current snapshot not selected" << endl;
		return (-1);
	}

	if (args.size() != 2) {
		cout << "Bad arguments" << endl;
		return (-1);
	}

	int ino  = strtol(args[1].c_str(), NULL, 10);


	auto inode = sb->getInodeFile();
	delete inode;

	if (currinode != nullptr) {
		delete currinode;
	}

	currinode = inode->getFile(ino);
	if (currinode == nullptr) {
		cout << "Problem inode" << endl;
		return (-1);
	}

	cout << currinode->toString() << endl;
	return (0);
}


int 
listinodes(Snapshot *sb, vector<string> &args)
{
	if (sb == nullptr) {
		cout << "Current snapshot not selected" << endl;
		return (-1);
	}

	auto inode = sb->getInodeFile();

	for (auto k : inode->availableInodes()) {
		cout << k.first << " -> (" << k.second.offset;
		cout << ", " << k.second.epoch << ")" << endl;
	}

	delete inode;

	return (0);
}

int 
dumpinode(Snapshot *sb, vector<string> &args)
{
	int error;
	if (currinode == nullptr) {
		cout << "No inode selected" << endl;
		return (-1);
	}

	if (args.size() != 2) {
		cout << "Bad arguments" << endl;
		return (-1);
	}

	error = currinode->dumpTo(args[1]);
	if (error) {
		return (error);
	}

	cout << "Dumped to " << args[1] << endl;
	return (0);
}

int 
printinode(Snapshot *sb, vector<string> &args)
{
	int error;
	if (currinode == nullptr) {
		cout << "No inode selected" << endl;
		return (-1);
	}

	cout << "=== Start File ===" << endl;
	cout << *currinode;
	cout << "=== End File ===" << endl;
	return (0);
}



typedef int (*cmd_t)(Snapshot *, vector<string> &);
extern std::map<string, std::pair<cmd_t, string>> cmds;

int
printhelp(Snapshot *, vector<string> &args)
{	
	cout << "Commands to use:" << endl;
	cout << "===============" << endl;
	for (auto k : cmds) {
		cout << setw(20) << left;
		cout << k.first;
		cout << k.second.second <<endl;
	}
	return (0);
}

std::map<string, std::pair<cmd_t, string>> cmds = {
    { "help", std::make_pair(printhelp, "Print help for commands") },
    { "ls", std::make_pair(listsnaps, "List snapshots on disk") },
    { "snap", std::make_pair(selectsnap, "Select snapshot to work with (Ex. snap NUM)") },
    { "li", std::make_pair(listinodes, "List all inodes Ex. INO_NUM -> (BLKNUM, EPOCH)") },
    { "inode", std::make_pair(selectinode, "Select an inode (Ex. inode NUM)") },
    { "dump", std::make_pair(dumpinode, "Dump current selected inode to path (Ex. dump path/to/dump.txt)") },
    { "pi", std::make_pair(printinode, "Print current selected inode to path (Ex. pi)") }
};

static char *
fsdb_prompt(EditLine *el)
{
	static char prompt[] = "fsdb> ";
	return (prompt);
}

HistEvent ev;

static void
fsdb_cli(void)
{
	const char **argv;
	HistEvent ev;
	int num, idx;
	int status;
	int argc;
	int error;

	History *hist = history_init();
	history(hist, &ev, H_SETSIZE, 100);

	 EditLine *el = el_init("fsdb", stdin, stdout, stderr);

	/* Editing mode to use. */
	el_set(el, EL_EDITOR, "vi");

	/* History function to use. */
	el_set(el, EL_HIST, history, hist);

	/* Function that displays the prompt. */
	el_set(el, EL_PROMPT, fsdb_prompt);

	for (;;) {
		/* Get the line. */
		const char *line = el_gets(el, &num);

		/* Tokenize the next line. */
		history(hist, &ev, H_ENTER, line);

		Tokenizer *tok = tok_init(NULL);
		argc = MAX_ARGC;
		status = tok_str(tok, line, &argc, &argv);
		if (status != 0) {
			fprintf(stderr, "Parsing error %d\n", status);
			tok_end(tok);
			continue;
		}

		assert(argc >= 0);
		/* Empty line, we're done. */
		if (argc == 0) {
			tok_end(tok);
			continue;
		}

		vector<string> args;
		for (int i = 0; i < argc; i++) {
			args.push_back(string { argv[i] } );
		}

		if (args[0] == "q") {
			break;
		}

		if (cmds.find(args[0]) != cmds.end()) {
			error = cmds[args[0]].first(curr, args);
			if (error) {
				cout << "Error: " << error << endl;
			}
		} else {
			cout << "Incorrect cmd" << endl;
		}


		/* Run the command. */
		tok_end(tok);
	}

	el_end(el);
	history_end(hist);
}



int
main(int argc, char *argv[])
{
	struct stat stats;
	int error;

	if (argc != 2) {
		cout << "Invalid number of arguments" << endl;
		return (-1);
	}


	dev = open(argv[1], O_RDONLY);
	if (dev == -1) {
		cout << strerror(errno) << endl;
		return (-1);
	}

	error = fstat(dev, &stats);
	if (error) {
		cout << strerror(errno) << endl;
		return (-1);
	}

	
	cout << "=== Starting FSDB ===" << endl;
	cout << endl;

	sectorsize = 512;
	blksize = stats.st_blksize;

	cout << "Device Information" << endl;
	cout << "==================" << endl;
	cout << "Sector size: " << sectorsize << "B" << endl;
	cout << "Block size: " << blksize << "B" << endl;
	cout << endl;

	vector<string> a;
	printhelp(nullptr, a);
	cout << endl;

	fsdb_cli();

	return (0);
}
