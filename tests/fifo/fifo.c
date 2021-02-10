#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define PATH ("testfifo")
#define MSG ("Message")
char buf[sizeof(MSG)];

int
main(int argc, char *argv[])
{
	int error;
	pid_t pid;
	int fd;

	if (argc != 2) {
		printf("Usage:./fifo basedir\n");
		exit(1);
	}

	error = chdir(argv[1]);
	if (error < 0) {
		perror("chdir");
		exit(1);
	}

	error = mkfifo(PATH, O_RDWR);
	if (error < 0 && errno != EEXIST) {
		perror("mkfifo");
		exit(1);
	}

	fd = open(PATH, O_RDWR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (pid == 0) {
		/* The child writes. */
		sleep(120);
		write(fd, MSG, sizeof(MSG));
		exit(0);
	}

	/*
	 * The parent tries to read immediately, gets stuck. It should correctly
	 * try to read after the restore.
	 */

	error = read(fd, buf, sizeof(MSG));
	if (error < 0) {
		perror("read");
		exit(1);
	}

	return (0);
}
