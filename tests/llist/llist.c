#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#define PAGE_SIZE 4096
#define PAGE_NUM 128
#define ROW_SIZE 16

#define PAGE_MIDDLE(pages, i) (pages + (PAGE_SIZE * i) + (PAGE_SIZE / 2))

struct llist {
    int cnt;
    struct llist *next;
};

int main() {
	struct llist *cur, *head;
	void *pages;
	int cnt;
	int i;

	pages = malloc(PAGE_SIZE * PAGE_NUM);
	if (pages == NULL) {
	    printf("array malloc failed\n");
	    return 0;
	}

	head = PAGE_MIDDLE(pages, 0);
	head->cnt = 0;

	cur = head;
	for (i = 0; i < PAGE_NUM - 2; i++) {
	    cur->next = PAGE_MIDDLE(pages, i);
	    cur->cnt = 0;

	    cur = cur->next;
	}

	cur->next = head;
	cur->cnt = 0;

	cnt = 0;
	for (cur = head; true; cur = cur->next) {
	    if (cur == head)
		cnt++;
	    cur->cnt++;
	    if (cur->cnt != cnt) {
		printf("Memory corruption!\n");
		fflush(stdout);
	    }
	}

	return 0;
}
