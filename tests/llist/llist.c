#include <sys/mman.h>

#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define ROW_SIZE 16
#define ITERATIONS 1000000

#define PAGE_SIZE 4096

struct llist {
	long cnt;
	struct llist *next;
};

#define PAGE_START(pages, i) (pages + (PAGE_SIZE * i))
#define PAGE_MIDDLE(pages, i) (pages + (PAGE_SIZE * i) + (PAGE_SIZE / 2))
#define PAGE_END(pages, i) \
	(pages + (PAGE_SIZE * (i + 1)) - sizeof(struct llist))

const long BILLION = 1000L * 1000 * 1000;

long
tonano(struct timespec time)
{
	return BILLION * time.tv_sec + time.tv_nsec;
}

int
main(int argc, char *argv[])
{
	struct llist *cur, *head;
	void *pages;
	long previous = 0;
	size_t megabytes;
	size_t page_number;
	struct timespec tstart = { 0, 0 }, tnow = { 0, 0 };
	long lastcnt, cnt;
	int i;

	if (argc != 2) {
		printf("Usage: ./llist <# of MBs>\n");
		return 0;
	}

	megabytes = strtol(argv[1], NULL, 10);
	if (megabytes == 0) {
		printf("Usage: ./llist <# of MBs>\n");
		return 0;
	}
	page_number = megabytes * (1024 * 1024 / 4096);

	pages = mmap(NULL, 1024 * 1024 * megabytes, PROT_READ | PROT_WRITE,
	    MAP_ANON, -1, 0);
	if (pages == NULL) {
		printf("array malloc failed\n");
		return 0;
	}

	head = PAGE_MIDDLE(pages, 0);
	head->cnt = 0;

	cur = head;
	for (i = 0; i < page_number - 2; i++) {
		cur->next = PAGE_MIDDLE(pages, i);
		cur->cnt = 0;

		cur = cur->next;
	}

	cur->next = head;
	cur->cnt = 0;

	printf("MBs malloc'd: %ld\n", megabytes);

	clock_gettime(CLOCK_MONOTONIC, &tstart);
	lastcnt = cnt = 0;
	for (cur = head; cnt < ITERATIONS; cur = cur->next) {
		if (cur == head) {
			cnt++;
			clock_gettime(CLOCK_MONOTONIC, &tnow);
			if (tonano(tnow) - tonano(tstart) >= BILLION) {
				printf(
				    "Rounds per second: %lu\n", cnt - lastcnt);
				lastcnt = cnt;
				clock_gettime(CLOCK_MONOTONIC, &tstart);
			}
		}
		cur->cnt++;
		if (cur->cnt != cnt) {
			printf(
			    "Memory corruption in %p! Expected %ld, got %ld\n",
			    cur, cnt, cur->cnt);
			printf("Previous: %ld\t Next: %ld\n", previous,
			    cur->next->cnt);
			/*
			bool first_time = true;
			int i = 0;
			for (cur = head; first_time || cur != head; cur =
			cur->next) { first_time = false; printf("%d ",
			cur->cnt); if (i++ % 16 == 0) printf("\n");
			}
			fflush(stdout);
			*/
			exit(1);
		}
		previous = cur->cnt;
	}

	return 0;
}
