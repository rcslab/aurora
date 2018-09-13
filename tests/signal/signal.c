#include <stdio.h>
#include <stdlib.h>

#include <signal.h>

#define CYCLES (1000 * 1000 * 1000)
void handle_sigusr(int signal)
{
	printf("SIGUSR1 handler called successfully.\n");
}

int main()
{

	struct sigaction sa;
	int i;

	sa.sa_handler = handle_sigusr;
	sa.sa_flags = SA_RESTART;

	sigfillset(&sa.sa_mask);


	if (sigaction(SIGUSR1, &sa, NULL) < 0) {
		printf("Setting up SIGUSR failed.\n");

	}


	for (;;) {
		for (i = 0; i < CYCLES; i++)
			;

		printf("Program still here.\n");
	}

	return 0;

}
