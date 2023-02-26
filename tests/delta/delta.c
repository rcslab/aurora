#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_SIZE (4096)

char buf[PAGE_SIZE];

void
modify()
{
	char val = (rand() % ('z' - 'a')) + 'a';
	int i;

	memset(buf, val, PAGE_SIZE);
	for (i = 0; i < PAGE_SIZE; i++) {
		if (buf[i] != val) {
			printf("INCONSISTENCY\n");
			exit(1);
		}
	}
	memset(buf, 0, PAGE_SIZE);
	usleep(500 * 1000);
	printf("CONSISTENT\n");
}

int
main()
{
	for (;;)
		modify();

	return (0);
}
