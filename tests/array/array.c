#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#define ARRAY_SIZE 8

int arr[ARRAY_SIZE];

int main() {
	printf("%d\n", getpid());
	memset(arr, 0x3f, sizeof(arr));
	printf("%x\n", (unsigned int)arr);
	for (int i = 0; i < 20; i ++) {
		sleep(1);
		printf("%d %x\n", i, arr[i%ARRAY_SIZE]++);
	}
	return 0;
}
