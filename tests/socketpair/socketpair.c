#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

int main(void)
{
	pid_t pid;
	int error;
	int sv[2]; /* the pair of socket descriptors */
	char buf; /* for data exchange between processes */

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
	    perror("socketpair");
	    exit(0);
	}

	pid = fork();
	if (pid < 0) {
	    perror("fork");
	    exit(0);
	}

	if (pid > 0) {  
	    for (;;)  {
		error = read(sv[1], &buf, 1);
		if (error == -1) {
		    perror("read");
		    continue;
		}

		printf("%c\n", buf);
	    }

	} else {
	    for (;;) {
		error = write(sv[0], "b", 1);
		if (error == -1) {
		    perror("write");
		    continue;
		}

		sleep(1);
	    }

	}

	return 0;
}
