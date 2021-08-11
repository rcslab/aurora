#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define NUMCHILDREN (1)
#define SLEEPTIME (20)
#define STOPTIME (5)

bool should_be_signalled;

void
handle_sigusr(int signal)
{
	int ret;

	printf("(%d) Got signalled\n", getpid());
	ret = should_be_signalled ? 0 : 1;
	exit(ret);
}

void
set_handler(void (*func)(int))
{
	struct sigaction sa;

	sa.sa_handler = func;
	sa.sa_flags = SA_RESTART;

	sigfillset(&sa.sa_mask);

	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		printf("Setting up SIGUSR failed.\n");
		exit(1);
	}

	printf("%d handler set\n", getpid());
}

void
ignore_sigusr(void)
{
	struct sigaction sa;

	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;

	sigfillset(&sa.sa_mask);

	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		printf("Setting up SIGUSR failed.\n");
		exit(1);
	}

	printf("%d SIGUSR1 ignore set\n", getpid());
}

void
same_pgroup(void)
{
	int error;

	// printf("(%d) Into same_pgroup\n", getpid());
	set_handler(&handle_sigusr);

	/* Two sleeps because one is interrupted by the checkpoint. */
	sleep(SLEEPTIME);
	sleep(STOPTIME + 2);

	/* Should receive a signal.*/
	exit(1);
}

void
different_pgroup(void)
{
	int error;

	/* Set the handler because if we receive a signal something went wrong.
	 */
	set_handler(&handle_sigusr);

	error = setpgid(0, 0);
	if (error != 0) {
		perror("setpgid");
		exit(1);
	}

	sleep(SLEEPTIME);
	sleep(STOPTIME + 2);
	printf("(%d) Didn't get signalled\n", getpid());

	exit(0);
}

int
main()
{
	int pid_different_pgroup[NUMCHILDREN];
	int pid_same_pgroup[NUMCHILDREN];
	int pid, status;
	int childpid;
	int error;
	int i;

	error = setpgid(0, 0);
	if (error != 0) {
		perror("setpgid");
		exit(1);
	}

	for (i = 0; i < NUMCHILDREN; i++) {
		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(1);
		}

		if (pid == 0) {
			should_be_signalled = true;
			same_pgroup();
		}

		pid_same_pgroup[i] = pid;
	}

	for (i = 0; i < NUMCHILDREN; i++) {
		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(1);
		}

		if (pid == 0) {
			should_be_signalled = false;
			different_pgroup();
		}

		pid_different_pgroup[i] = pid;
	}

	ignore_sigusr();

	/* Wait to be checkpointed. */
	sleep(SLEEPTIME);
	sleep(STOPTIME);

	kill(0, SIGUSR1);

	for (i = 0; i < 2 * NUMCHILDREN; i++) {
		childpid = wait(&status);
		if (childpid < 0) {
			perror("wait");
			exit(1);
		} else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
			fprintf(stderr, "Process %d exited with status %d\n",
			    childpid, WEXITSTATUS(status));
			exit(2);
		} else if (WIFSIGNALED(status) && WEXITSTATUS(status) != 0) {
			fprintf(stderr,
			    "Process %d terminated with signal %d and status %d\n",
			    childpid, WTERMSIG(status), WEXITSTATUS(status));
			exit(4);
		}
	}

	printf("Successfully waited\n");
	return (0);
}
