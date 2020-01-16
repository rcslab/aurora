#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#define PATH	("slsfifo")
#define MSG	("Message")
char buf[sizeof(MSG)];

int
main(void)
{
	int error;
	pid_t pid;
	int fd;

	error = mkfifo(PATH, O_RDWR);
	if (error < 0) {
	    perror("mkfifo");
	    /* Keep going, not cleaning up not really an error. */
	}

	fd = open(PATH, O_RDWR);
	if (fd < 0) {
	    perror("open");
	    exit(0);
	}

	pid = fork();
	if (pid < 0) {
	    perror("fork");
	    exit(0);
	}

	if (pid > 0) {
	    for (;;) {
		write(fd, MSG, sizeof(MSG));
		sleep(1);
		printf("Got %s\n", MSG);
	    }
	} else {
	    for (;;) {
		sleep(1);
		read(fd, MSG, sizeof(MSG));
	    }
	}

	return (0);
}
