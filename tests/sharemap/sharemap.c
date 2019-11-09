#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <machine/atomic.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define FILE_SIZE (128 * 4096)

void *sharemap;

void
ping()
{
	int ret;

	printf("%d\n", getpid());

	for (;;) {
	    sleep(1);

	    while (atomic_cmpset_int((int *) sharemap, 0, 1) == 0)
		    ;

	    printf("ping\n");
	}
}

void
pong()
{
	int ret; 

	printf("%d\n", getpid());

	for (;;) {
	    sleep(1);

	    while (atomic_cmpset_int((int *) sharemap, 1, 0) == 0)
		    ;

	    printf("pong\n");
	}
}

int
main(int argc, char **argv)
{
	pid_t pid;
	int error;

	if (argc != 1) {
		printf("Usage: ./sharemap\n");
		return 0;
	}

	sharemap = mmap(NULL, FILE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_ANON, -1, 0);

	if (sharemap == MAP_FAILED) {
	    perror("mmap");
	    return (0);
	}

	printf("%p\n", sharemap);
	printf("%d\n", *((int *) sharemap));


	*((int *)sharemap) = 0;

	pid = fork();
	if (pid == 0)
	    pong();
	else if (pid > 0)
	    ping();
	else
	    perror("fork");

	return (0);
}
