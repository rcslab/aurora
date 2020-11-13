#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#define MSG	("Message")
char buf[sizeof(MSG)];

int
main(int argc, char *argv[])
{
	int error;
	int pipes[2];
	pid_t pid;

	error = pipe(pipes);
	if (error < 0) {
	    perror("pipe");
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
		write(pipes[1], MSG, sizeof(MSG));
		exit(0);
	}

	/*
	 * The parent tries to read immediately, gets stuck. It should correctly
	 * try to read after the restore.
	 */

	error = read(pipes[0], buf, sizeof(MSG));
	if (error < 0) {
		perror("read");
		exit(1);
	}

	return (0);
}
