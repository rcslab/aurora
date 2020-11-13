#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

void
childfunction(void)
{
	sleep(2);
	sleep(2);
	exit(0);
}

int
main() {
	int pid, status;
	int childpid;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (pid == 0)
		childfunction();

	childpid = wait(&status);
	if (childpid < 0) {
		perror("wait");
		exit(1);
	} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		fprintf(stderr, "Process exited with status %d\n", WEXITSTATUS(status));
		exit(2);
	} else if (WIFSIGNALED(status)) {
		fprintf(stderr, "Process terminated with signal %d\n", WTERMSIG(status));
		exit(4);
	}

	printf("Successfully waited\n");
	return(0);
}
