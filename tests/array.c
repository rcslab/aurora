#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

int arr[128];

int main() {
	printf("%d\n", getpid());
	memset(arr, 0x3f, sizeof(arr));
	printf("%x\n", (unsigned int)arr);
	for (int i = 0; i < 100; i ++) {
		sleep(1);
		printf("%d %x\n", i, arr[i%128]);
	}
	return 0;
}
