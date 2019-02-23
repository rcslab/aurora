#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#define PAGE_SIZE 4096
#define PAGE_NUM 128
#define ROW_SIZE 16

int main() {
	char **pages;
	int i;

	pages = malloc(sizeof(*pages) * PAGE_NUM);
	if (pages == NULL) {
	    printf("array malloc failed\n");
	    return 0;
	}
	for (i = 0; i < PAGE_NUM; i++) {
	    pages[i] = malloc(sizeof(**pages) * PAGE_SIZE);
	    if (pages[i] == NULL) {
		printf("page malloc failed\n");
		return 0;
	    }
	}

	for (i = 0; i < PAGE_NUM; i++) {
	    pages[i][0xFF] = i;
	}

	printf("%d\n", getpid());
	
	for (;;) {
	    sleep(1);
	    for (int i = 0; i < PAGE_NUM; i ++) {
		printf("%02x ", pages[i][0xFF]);
		if (i % ROW_SIZE == 0)
		    printf("\n");
	    }
	    printf("\n");
	}
	return 0;
}
