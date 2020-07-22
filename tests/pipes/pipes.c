#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

int parentpipe[2];
int childpipe[2];
uint64_t pingpong;

void
pipe_reader()
{
	int ret;
	int remaining;

	printf("%d\n", getpid());
	close(parentpipe[0]);
	close(childpipe[1]);

	for (;;) {

		remaining = sizeof(pingpong);
		while (remaining > 0) {
			ret = read(childpipe[0], &pingpong, remaining);
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;

				perror("read");
				exit(0);
			} else if (ret == 0) {
				printf("EOF");
				break;
			}
			remaining -= ret;
		}

		sleep(1);

		remaining = sizeof(pingpong);
		while (remaining > 0) {
			ret = write(parentpipe[1], &pingpong, remaining);
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;

				perror("write");
				exit(0);
			} else if (ret == 0) {
				printf("EOF");
				break;
			}
			remaining -= ret;
		}

		printf("pong\n");

	}
}

void
pipe_writer()
{
	int ret;
	int remaining;

	printf("%d\n", getpid());
	close(childpipe[0]);
	close(parentpipe[1]);

	for (;;) {
		sleep(1);

		remaining = sizeof(pingpong);
		while (remaining > 0) {
			ret = write(childpipe[1], &pingpong, remaining);
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;

				perror("write");
				exit(0);
			} else if (ret == 0) {
				printf("EOF");
				break;
			}
			remaining -= ret;
		}

		printf("ping\n");

		remaining = sizeof(pingpong);
		while (remaining > 0) {
			ret = read(parentpipe[0], &pingpong, remaining);
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;

				perror("read");
				exit(0);
			} else if (ret == 0) {
				printf("EOF");
				break;
			}
			remaining -= ret;
		}

	}
}

int
main(int argc, char **argv)
{

	pid_t pid;
	int error;

	if (argc != 1) {
		printf("Usage: ./pipes\n");
		return 0;
	}

	error = pipe(parentpipe);
	if (error != 0) {
		perror("pipe");
		return 0;
	}

	error = pipe(childpipe);
	if (error != 0) {
		perror("childpipe");
		return 0;
	}

	pid = fork();
	if (pid == 0)
		pipe_writer();
	else if (pid > 0)
		pipe_reader();
	else
		perror("pipe");

	return 0;
}
