#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define ARRAY_SIZE (8)
#define ITERATIONS (300)

int arr[ARRAY_SIZE];

int
main()
{
	printf("%d\n", getpid());
	memset(arr, 0x3f, sizeof(arr));
	printf("%x\n", (unsigned int)arr);
	for (int i = 0; i < ITERATIONS; i++) {
		sleep(1);
		printf("%d %x\n", i, arr[i % ARRAY_SIZE]++);
	}
	return 0;
}
