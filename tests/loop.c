#include <stdio.h>

int main()
{
	int i;

	for (i = 0; /* true */; i++) {
		printf("%d\n", i);
		sleep(1);
	}
	return 0;
}

