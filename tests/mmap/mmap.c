#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#define min(a, b) ((a < b) ? (a) : (b))

#define PAGE_SIZE (4096)
#define FILE_SIZE (PAGE_SIZE * 64)


const char value[] = "0xdeadbeef";

int
main(int argc, char **argv)
{
	int error;
	void *file_mapping;
	int i;
	int fd;

	if (argc != 2) {
		printf("Usage: ./mmap <file>\n");
		return 0;
	}

	fd = open(argv[1], O_RDWR | O_TRUNC | O_CREAT);
	if (fd < 0) {
		printf("Error: open failed\n");
		return -1;
	}

	error = lseek(fd, FILE_SIZE - 1, SEEK_SET);
	assert(error > 0);

	error = write(fd, "", 1);
	assert(error >= 0);

	file_mapping = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (file_mapping == MAP_FAILED) {
		printf("Error: mmap failed\n");
		return -1;
	}

	printf("Mapping received: %p\n", file_mapping);

	printf("Press any key to copy...\n");
	getchar();
	sleep(10);
	
	for (i = 0; i < FILE_SIZE; i += sizeof(value)) {
		memcpy(&file_mapping[i], value, min(FILE_SIZE - i, sizeof(value)));
	}

	printf("Press any key to exit...\n");
	getchar();
	
	return 0;
}
