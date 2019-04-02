#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

#define ARRAY_SIZE 8

int arr[ARRAY_SIZE];

int main() {
	int pid, status;
	printf("%d\n", getpid());
	memset(arr, 0x3f, sizeof(arr));
	printf("%x\n", (unsigned int)arr);
	for (int i = 0; i < 100; i ++) {
		sleep(1);
		printf("%d %x\n", i, arr[i%ARRAY_SIZE]++);
		if ((pid = fork()) == 0)
			exit(0);
		waitpid(-1, &status, 0);
	}
	return 0;
}
