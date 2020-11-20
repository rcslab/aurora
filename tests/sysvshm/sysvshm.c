#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <machine/atomic.h>

#define SHM_SIZE    (4096)

char *
getsysvshm(key_t key, int *shmid, int flag)
{
	char *shm;

	*shmid = shmget(key, SHM_SIZE, flag | 0666);
	if (*shmid < 0) {
		shmctl(*shmid, IPC_RMID, NULL);
		perror("shmget");
		exit(1);
	}

	/*
	 * Temporarily attach the segment to initialize the first byte,
	 * which we will be using it for arbitrating access to the buffer.
	 */
	shm = (char *) shmat(*shmid, 0, 0);
	if (shm == (void *) -1) {
		perror("shmat");
		shmctl(*shmid, IPC_RMID, NULL);
		exit(1);
	}

	return (shm);
}


int
main()
{
	char path[1024];
	key_t key;
	char *shm;
	int shmid;
	int error;
	char *ret;
	char c;
	int i;

	ret = getcwd(path, 1024);
	if (ret == NULL) {
		perror("getcwd");
		exit(1);
	}

	key = ftok(path, 0);
	if (key < 0) {
		perror("ftok");
		exit(1);
	}

	shm = getsysvshm(key, &shmid, IPC_CREAT);

	c = 'a' + (random() % ('z' - 'a' + 1));
	memset(shm, c, SHM_SIZE);

	error = shmdt(shm);
	if (error != 0) {
		perror("shmdt");
		shmctl(shmid, IPC_RMID, NULL);
		exit(1);
	}

	sleep(200);

	shm = getsysvshm(key, &shmid, 0);
	for (i = 0; i < SHM_SIZE; i++) {
		if (shm[i] != c) {
			printf("Found byte '%d' instead of %d", shm[i], c);
			shmdt(shm);
			shmctl(shmid, IPC_RMID, NULL);
			exit(1);
		}
	}

	error = shmdt(shm);
	if (error != 0) {
		perror("shmdt");
		shmctl(shmid, IPC_RMID, NULL);
		exit(1);
	}

	error = shmctl(shmid, IPC_RMID, NULL);
	if (error != 0) {
		perror("shmctl");
		exit(1);
	}
	printf("SYSV test succeeded\n");

	return (0);
}
