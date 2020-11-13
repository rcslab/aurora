#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <signal.h>
#include <machine/sysarch.h>

#define SIGMSG ("SIGUSR1 handler called successfully.\n")

void handle_sigusr(int signal)
{
	char *msg = SIGMSG;

	write(STDOUT_FILENO, SIGMSG, sizeof(SIGMSG));
	exit(0);
}

int main()
{
	struct sigaction sa;
	int i;
	void *addr;

	sa.sa_handler = handle_sigusr;
	sa.sa_flags = SA_RESTART;

	sigfillset(&sa.sa_mask);

	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		printf("Setting up SIGUSR failed.\n");
		exit (1);
	}

	sleep(5);

	printf("Waiting for the signal...\n");

	sleep(5);
	printf("Signal never arrived\n");

	exit(1);
}
