#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CYCLES (100 * 1000 * 1000L)

#define BOGUS_CONSTANT (0xFF5)

int
main()
{
	long long i, j;
	int bogus;

	bogus = BOGUS_CONSTANT;
	for (i = 0; i >= 0; i++) {
		for (j = 0; j < CYCLES; j++)
			/*
			 * Bogus computation
			 * to avoid optimizing
			 * out the loop
			 */
			bogus = (bogus * BOGUS_CONSTANT) % 10;
		printf("%d %lld\n", bogus, i);
	}

	return 0;
}
