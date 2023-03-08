#include <sys/types.h>
#include <sys/event.h>
#include <sys/ipc.h>
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#include <assert.h>
#include <fcntl.h>
#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NUMEVENTS (1024)
#define SHM_SIZE (4096)
#define UNADDR ("localsocket")
#define UNADDR_MAX (108)
#define VNODETEST ("vnodetest")
#define SYSVPATH ("slssysvshm")

int shmid;
void *shm;
int unixfd;

void
usage(void)
{
	printf("Usage: ./posix <oid> <mountpoint>\n");
	exit(0);
}

void
teardown_sysvshm(void)
{
	int error;

	error = shmdt(shm);
	if (error != 0)
		perror("shmdt");

	error = shmctl(shmid, IPC_RMID, NULL);
	if (error != 0)
		perror("shmctl");
}

void
teardown_unixsocket(void)
{
	close(unixfd);
	/*
	 * We do not remove the socket, because right now we need
	 * to be able to find the vnode at restore time.
	 */
}

void
teardown_and_exit(void)
{
	teardown_sysvshm();
	teardown_unixsocket();
	exit(0);
}

void
setup_vnode(void)
{
	int fd;

	fd = open(VNODETEST, O_RDWR | O_CREAT, 0666);
	if (fd < 0) {
		perror("open");
		teardown_and_exit();
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
		teardown_and_exit();
	}

	/* Bind it to the address given in sockaddr_in. */
	error = bind(unixfd, (struct sockaddr *)&local, sizeof(local));
	if (error == -1) {
		perror("bind");
		teardown_and_exit();
	}

	error = listen(unixfd, 512);

	if (error == -1) {
		perror("listen");
		teardown_and_exit();
	}
}

void
setup_socketpair(void)
{
	int sv[2];

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == -1) {
		perror("socketpair");
		teardown_and_exit();
	}
}

void
setup_sysvshm(void)
{
	key_t key;

	key = ftok("/root", 0);
	if (key < 0) {
		perror("ftok");
		teardown_and_exit();
	}

	shmid = shmget(key, SHM_SIZE, IPC_CREAT | 0666);
	if (shmid < 0) {
		perror("shmget");
		teardown_and_exit();
	}

	/*
	 * Temporarily attach the segment to initialize the first byte,
	 * which we will be using it for arbitrating access to the buffer.
	 */
	shm = (char *)shmat(shmid, 0, 0);
	if (shm == (void *)-1) {
		perror("shmat");
		teardown_and_exit();
	}
}

void
setup_pipe(void)
{
	int error;
	int ppfd[2];

	error = pipe((int *)&ppfd);
	if (error != 0) {
		perror("pipe");
		teardown_and_exit();
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
		teardown_and_exit();
	}

	error = ftruncate(fd, getpagesize());
	if (error != 0) {
		perror("ftruncate");
		teardown_and_exit();
	}

	shm = (char *)mmap(
	    NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shm == MAP_FAILED) {
		perror("mmap");
		teardown_and_exit();
	}
}

void
setup_kqueue(void)
{
	struct kevent *kevs;
	int fd;
	int i;

	fd = kqueue();
	if (fd < 0)
		teardown_and_exit();

	kevs = malloc(NUMEVENTS * sizeof(*kevs));
	if (kevs == NULL) {
		perror("malloc");
		teardown_and_exit();
	}

	for (int i = 0; i < NUMEVENTS; i++)
		EV_SET(&kevs[i], i, EVFILT_USER, EV_ADD, 0, 0, 0);

	kevent(fd, kevs, NUMEVENTS, NULL, 0, NULL);
	free(kevs);
}

void
setup_pty(void)
{
	int fd;

	fd = posix_openpt(O_RDWR);
	if (fd < 0) {
		perror("posix_openpt");
		teardown_and_exit();
	}
}

int
main(int argc, char *argv[])
{
	struct sls_attr attr;
	char *mountpoint;
	int error, i;
	uint64_t oid;

	if (argc != 3)
		usage();

	oid = strtol(argv[1], NULL, 10);
	if (oid == 0) {
		printf("Invalid oid %s\n", argv[1]);
		exit(0);
	}

	mountpoint = argv[2];
	error = chdir(mountpoint);
	if (error != 0) {
		perror("chdir");
		teardown_and_exit();
	}

	setup_vnode();
	setup_pipe();
	setup_posixshm();
	setup_pty();
	setup_kqueue();
	setup_unixsocket();
	setup_socketpair();
	setup_sysvshm();

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		.attr_mode = SLS_DELTA,
		.attr_period = 0,
	};
	error = sls_partadd(oid, attr, -1);
	if (error != 0) {
		fprintf(stderr, "sls_partadd returned %d\n", error);
		teardown_and_exit();
	}

	error = sls_attach(oid, getpid());
	if (error != 0) {
		fprintf(stderr, "sls_attach returned %d\n", error);
		teardown_and_exit();
	}

	error = sls_checkpoint(oid, false);
	if (error != 0) {
		fprintf(stderr, "sls_checkpoint returned %d\n", error);
		teardown_and_exit();
	}

	teardown_and_exit();
	return (0);
}
