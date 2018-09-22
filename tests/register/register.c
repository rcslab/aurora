#include <stdio.h>
#include <stdint.h>

#define CYCLES (1000 * 1000 * 1000)


int main() 
{
	volatile register uint64_t rax asm ("rax");
	volatile register uint64_t rbx asm ("rbx");
	volatile register uint64_t rcx asm ("rcx");
	volatile register uint64_t rdx asm ("rdx");

	/* XXX We cannot use these registers it seems.
	volatile register uint16_t cs asm ("cs");
	volatile register uint16_t ds asm ("ds");
	volatile register uint16_t es asm ("es");
	volatile register uint16_t fs asm ("fs");
	volatile register uint16_t gs asm ("gs");
	volatile register uint16_t ss asm ("ss");
	*/

	int i;
	uint16_t fs;
	
	for (;;) {
		rax = rbx = rcx = rdx = 0;

		for (i = 0; i < CYCLES; i++) {
			rax += 1;
			rbx += 2;
			rcx += 3;
			rdx += 4;
		}

		asm volatile ("mov %%fs, %0\n\t"
			      : "=r" (fs)
		);


		printf("fs %u\n", fs);
		printf("rax %lu\trbx %lu\trax %lu\trdx %lu\t\n", rax, rbx, rcx, rdx);
		/*
		printf("cs %d\tds %d\tes %d\tfs %d\tgs %d\tss %d\t\n", cs, ds, es, fs, gs, ss);
		*/
	}

	return 0;
}
