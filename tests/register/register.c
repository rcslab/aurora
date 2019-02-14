#include <stdio.h>
#include <stdint.h>

#define CYCLES (1000 * 1000 * 1000)


int main() 
{
	volatile register uint64_t r13 asm ("r13");
	volatile register uint64_t r14 asm ("r14");
	volatile register uint64_t r15 asm ("r15");

	/* XXX We cannot use these registers it seems.
	volatile register uint16_t cs asm ("cs");
	volatile register uint16_t ds asm ("ds");
	volatile register uint16_t es asm ("es");
	volatile register uint16_t fs asm ("fs");
	volatile register uint16_t gs asm ("gs");
	volatile register uint16_t ss asm ("ss");
	*/

	uint16_t fs;
	
	for (;;) {
		r13 = r14 = 0;

		for (r15 = 0; r15 < CYCLES; r15++) {
			r13 += 1;
			r14 += 2;
		}

		asm volatile ("mov %%fs, %0\n\t"
			      : "=r" (fs)
		);


		printf("fs %u\n", fs);
		printf("r13 %lu\tr14 %lu\n", r13, r14);
	}

	return 0;
}
