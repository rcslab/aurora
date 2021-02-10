#include <stdio.h>
#include <unistd.h>

int
main()
{
	int i;

	for (i = 0; i >= 0; i++) {
		sleep(1);
		printf("%d\n", i);
	}

	return 0;
}
