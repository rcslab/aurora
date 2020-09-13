#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#include <sls.h>

#define OID (1000)
#define NUMFILES (16)
#define SHM_SIZE (4096) 
#define UNADDR ("/testmnt/localsocket")	
#define UNADDR_MAX (108)
#define VNODETEST ("/testmnt/vnodetest")
#define SYSVPATH ("/testmnt/slssysvshm")

int shmid;
void *shm;
int unixfd;

void
usage(void)
{
	printf("Usage: ./posix <mountpoint>\n");
	exit(0);
}

void
setup_vnode(void)
{
	int fd;

	fd = open(VNODETEST, O_RDWR | O_CREAT, 0666);
	if (fd < 0) {
	    perror("open");
	    exit(0);
	}
}

void
setup_unixsocket(void)
{
	struct sockaddr_un local, remote;
	socklen_t addrlen;
	int enable = 1;
	int error;

	local.sun_family = AF_UNIX;
	strlcpy(local.sun_path, UNADDR, UNADDR_MAX);

	/* Create the blank socket. */
	unixfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (unixfd == -1) {
	    perror("socket");
	    exit(0);
	}

	/* Bind it to the address given in sockaddr_in. */
	error = bind(unixfd, (struct sockaddr *) &local, sizeof(local));
	if (error == -1) {
	    perror("bind");
	    exit(0);
	}

	error = listen(unixfd, 512);

	if (error == -1) {
	    perror("listen");
	    exit(0);
	}
}

void
teardown_unixsocket(void)
{
	close(unixfd);
	remove(UNADDR);
}

void
setup_socketpair(void)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
	    perror("socketpair");
	    exit(0);
	}
}

void
setup_sysvshm(void)
{
	key_t key;

	key = ftok("/root", 0);
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
}

void
teardown_sysvshm(void)
{
	int error;

	error = shmdt(shm);
	if (error != 0) {
	    perror("shmdt");
	    exit(0);
	}

	error = shmctl(shmid, IPC_RMID, NULL);
	if (error != 0) {
		perror("shmctl");
		exit(0);
	}
}

void
setup_pipe(void)
{
	int error;
	int ppfd[2];

	error = pipe((int *) &ppfd);
	if (error != 0) {
		perror("pipe");
		exit(0);
	}
}

void
setup_posixshm(void)
{
	char *shm;
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
}

void
setup_pty(void)
{

}

int
main(int argc, char *argv[])
{
	struct sls_attr attr;
	char *mountpoint;
	int error, i;

	if (argc != 2)
		usage();

	mountpoint = argv[1];

	setup_vnode();
	setup_pipe();
	setup_posixshm();
	setup_pty();
	setup_unixsocket();
	setup_socketpair();
	setup_sysvshm();

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
	};
	error = sls_partadd(OID, attr);
	if (error != 0) {
		fprintf(stderr, "sls_partadd returned %d\n", error);
		exit(0);
	}

	error = sls_attach(OID, getpid());
	if (error != 0) {
		fprintf(stderr, "sls_attach returned %d\n", error);
		exit(0);
	}

	error = sls_checkpoint(OID, false, true);
	if (error != 0) {
		fprintf(stderr, "sls_checkpoint returned %d\n", error);
		exit(0);
	}

	error = sls_partdel(OID);
	if (error != 0) {
		fprintf(stderr, "sls_partdel returned %d\n", error);
		exit(0);
	}

	teardown_sysvshm();
	teardown_unixsocket();

	return (0);
}
