#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/mman.h>
		
#include <machine/atomic.h>

#define POSIXPATH   ("/fake/path/posixshm")
#define SHM_SIZE    (4096) 
#define PARENT_MSG  ("PARENT WAS HERE")
#define CHILD_MSG   ("CHILD WAS HERE")

int 
main()
{
	key_t key;
	pid_t pid;
	char *shm;
	char *buf;
	uint8_t *mtx;
	int error;
	int fd;

	fd = shm_open(SHM_ANON, O_RDWR | O_CREAT, 0666);
	if (fd < 0) {
	    perror("shm_open");
	    exit(0);
	}

	error = ftruncate(fd, getpagesize());
	if (error != 0) {
	    perror("ftruncate");
	    exit(0);
	}

	shm = (char *) mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
	    perror("mmap");
	    exit(0);
	}

	/* Initialization of the "mutex" (not really a mutex). */
	atomic_store_8((uint8_t *) shm, 0);

	pid = fork();
	if (pid < 0) {
	    perror("fork");
	    exit(0);
	}
		
	/* The "mutex" is in the first byte of the segment, the rest holds data. */
	mtx = (uint8_t *) shm;
	buf = &shm[1];

	for (;;) {
	    /* Get the "mutex". */
	    while (atomic_cmpset_8(mtx, 0, 1) == 0)
		sleep(1);

	    if (pid > 0)
		strlcpy(buf, PARENT_MSG, sizeof(PARENT_MSG));
	    else
		strlcpy(buf, CHILD_MSG, sizeof(CHILD_MSG));

	    sleep(1);

	    /* Leave the critical section. */
	    atomic_store_8(mtx, 0);

	    sleep(1);
	    
	    while (atomic_cmpset_8(mtx, 0, 1) == 0)
		sleep(1);

	    printf("Reading from %s: %s\n", (pid > 0) ? "parent" : "child", buf);

	    atomic_store_8(mtx, 0);
	}

	return 0;
}
