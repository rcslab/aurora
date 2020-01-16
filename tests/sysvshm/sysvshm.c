#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
		
#include <machine/atomic.h>

#define SYSVPATH    ("slssysvshm")
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
	int shmid;
	int error;

	key = ftok(SYSVPATH, 0);
	if (key < 0) {
	    perror("ftok");
	    exit(0);
	}

	shmid = shmget(key, SHM_SIZE, IPC_CREAT | 0666);
	if (shmid < 0) {
	    perror("shmget");
	    exit(0);
	}

	/* 
	 * Temporarily attach the segment to initialize the first byte,
	 * which we will be using it for arbitrating access to the buffer.
	 */
	shm = (char *) shmat(shmid, 0, 0);
    	if (shm == (void *) -1) {
	    perror("shmat");
	    exit(0);
	}

	/* Initialization of the "mutex" (not really a mutex). */
	atomic_store_8((uint8_t *) shm, 0);

	error = shmdt(shm);
	if (error != 0) {
	    perror("shmdt");
	    exit(0);
	}

	pid = fork();
	if (pid < 0) {
	    perror("fork");
	    exit(0);
	}
		
	/* The "mutex" is in the first byte of the segment, the rest holds data. */
	mtx = (uint8_t *) shm;
	buf = &shm[1];

	/* Both processes independently attach _after_ the fork. */
	shm = (char *) shmat(shmid, 0, 0);
    	if (shm == (void *) -1) {
	    perror("shmat");
	    exit(0);
	}

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
