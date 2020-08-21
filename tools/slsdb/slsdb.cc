#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>
#include <histedit.h>

#include <iostream>
#include <vector>
#include <sstream>
#include <iomanip>
#include <map>

#include <slsfs.h>
#include <slos.h>
#include <btree.h>
#include <slos_inode.h>

#include "btree.h"
#include "snapshot.h"
#include "file.h"
#include "util.h"

using namespace std;

#define MAX_ARGC (32)
typedef int (*cmd_t)(Snapshot *, vector<string> &);

int dev;
size_t blksize;
size_t sectorsize;
int err = 0;
vector<Snapshot> snaps;
Snapshot * curr = nullptr;
std::shared_ptr<SFile> currinode = nullptr;

int cmd_help(Snapshot *, vector<string> &args);

int 
retrieveSnaps(vector<Snapshot> &snaps)
{
	char buf[SECTOR_SIZE] = {};
	struct slos_sb sb;
	int readin;

	if (snaps.size() != 0) {
		return (0);
	}

	for (int i = 0; i < NUMSBS; i++) {
		off_t offset = i * sectorsize;
		readin = pread(dev, buf, sectorsize, offset);
		if (readin != sectorsize) {
			cout << strerror(errno) << " " << errno << endl;
			return (-1);
		}

		memcpy(&sb, buf, sizeof(struct slos_sb));

		if (sb.sb_magic != SLOS_MAGIC) {
			//cout << "Corrupted superblock at " << i << endl;
		}

		if (sb.sb_epoch != EPOCH_INVAL) {
			snaps.push_back(Snapshot { dev, sb });
		} else {
			break;
		}
	}
	return (0);
};

uint64_t
lastsnap()
{
	int error;
	uint64_t max = 0;

	error = retrieveSnaps(snaps);
	if (error) {
		return (error);
	}

	for (auto k : snaps) {
		max = std::max(k.super.sb_epoch, max);
	}
	
	return max;
}

int 
cmd_ls(Snapshot *sb, vector<string> &args)
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
cmd_snap(Snapshot *sb, vector<string> &args)
{
	int error;
	int snap;

	if (args.size() != 2) {
		cout << "Bad arguments" << endl;
		return (-1);
	}

	snap = strtol(args[1].c_str(), NULL, 10);

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
cmd_inode(Snapshot *sb, vector<string> &args)
{
	int ino;

	if (sb == nullptr) {
		cout << "Current snapshot not selected" << endl;
		return (-1);
	}

	if (args.size() != 2) {
		cout << "Bad arguments" << endl;
		return (-1);
	}

	ino = strtol(args[1].c_str(), NULL, 10);

	auto inode = sb->getInodeFile();
	if (!inode) {
		return (-1);
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
cmd_li(Snapshot *sb, vector<string> &args)
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

	return (0);
}

int 
cmd_print(Snapshot *sb, vector<string> &args)
{
	if (currinode == nullptr) {
		cout << "No inode selected" << endl;
		return (-1);
	}

	currinode->print();

	return (0);
}

int 
cmd_dump(Snapshot *sb, vector<string> &args)
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
cmd_hexdump(Snapshot *sb, vector<string> &args)
{
	if (currinode == nullptr) {
		cout << "No inode selected" << endl;
		return (-1);
	}

	currinode->hexdump();

	return (0);
}

int
cmd_exit(Snapshot *unused, vector<string> &args)
{
	exit(0);
}

std::map<string, std::pair<cmd_t, string>> cmds = {
    { "ls", std::make_pair(cmd_ls, "List snapshots") },
    { "snap", std::make_pair(cmd_snap, "Select a snapshot") },
    { "li", std::make_pair(cmd_li, "List inodes") },
    { "inode", std::make_pair(cmd_inode, "Select an inode") },
    { "print", std::make_pair(cmd_print, "Print inode") },
    { "dump", std::make_pair(cmd_dump, "Dump inode to file") },
    { "hexdump", std::make_pair(cmd_hexdump, "Hexdump inode") },
    { "help", std::make_pair(cmd_help, "Show help") },
    { "exit", std::make_pair(cmd_exit, "Exit the program") }
};

int
cmd_help(Snapshot *, vector<string> &args)
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

static char *
slsdb_prompt(EditLine *el)
{
	static char prompt[] = "slsdb> ";

	return (prompt);
}

static void
slsdb_cli(void)
{
	const char **argv;
	HistEvent ev;
	int num, idx;
	int status;
	int argc;
	int error;

	sigset_t mask;
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, nullptr);

	History *hist = history_init();
	history(hist, &ev, H_SETSIZE, 100);

	EditLine *el = el_init("slsdb", stdin, stdout, stderr);

	/* Editing mode to use. */
	el_set(el, EL_EDITOR, "vi");

	/* History function to use. */
	el_set(el, EL_HIST, history, hist);

	/* Function that displays the prompt. */
	el_set(el, EL_PROMPT, slsdb_prompt);

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

void
usage()
{
	cout << "Usage: slsdb DEVICE" << endl;
}

int
main(int argc, char **argv)
{
	struct stat stats;
	int error;

	if (argc < 2) {
		usage();
		return (1);
	}


	dev = open(argv[argc - 1], O_RDONLY);
	if (dev == -1) {
		perror("open");
		return (1);
	}

	error = fstat(dev, &stats);
	if (error) {
		perror("fstat");
		return (1);
	}

	sectorsize = 512;
	blksize = stats.st_blksize;

	int c = 0;
	while ((c = getopt(argc, argv, "s")) != -1) {
		switch (c) {
			case 's':
				cout << lastsnap() << endl;
				return (0);
			default:
				break;
		}
	}

	cout << "=== Starting FSDB ===" << endl;
	cout << "Device Information" << endl;
	cout << "==================" << endl;
	cout << "Sector size: " << sectorsize << "B" << endl;
	cout << "Block size: " << blksize << "B" << endl;
	cout << endl;

	vector<string> a;
	cmd_help(nullptr, a);
	cout << endl;

	slsdb_cli();

	return (0);
}
