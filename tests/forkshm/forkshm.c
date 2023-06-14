#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KB (1024)
#define MB (1024 * KB)
#define GB (1024 * MB)

#define SIZE (10 * MB)

#define MSG ("Scrawling all over the SAS mapping")

#define PATH ("/sasfile")
/*
 * CAREFUL: this needs to be a divisor of 4096 (PAGE_SIZE)
 * and larger than strnlen(MSG) for the test to work.
 */
#define ITERS (128)

void
kick_reader(int fd)
{
	ssize_t ret;
	char c;

	ret = write(fd, &c, sizeof(c));
	if (ret < 0) {
		perror("write");
		exit(1);
	}

	if (ret == 0) {
		fprintf(stderr, "zero byte write\n");
		exit(1);
	}
}

void
waitfor_writer(int fd)
{
	ssize_t ret;
	char c;

	ret = read(fd, &c, sizeof(c));
	if (ret < 0) {
		perror("read");
		exit(1);
	}

	if (ret == 0) {
		fprintf(stderr, "zero byte read\n");
		exit(1);
	}
}

void
read_mapping(void *sas, int readfd)
{
	char buf[sizeof(MSG)];
	void *addr;
	int i;

	for (i = 0; i < ITERS; i++) {
		waitfor_writer(readfd);
		addr = &sas[i * (SIZE / ITERS)];
		if (strncmp(addr, MSG, sizeof(MSG)) != 0) {
			printf("Improper read (%s) (%s)\n", addr, MSG);
			exit(1);
		}
	}
}

void
write_mapping(void *sas, int writefd)
{
	void *addr;
	int i;

	for (i = 0; i < ITERS; i++) {
		addr = &sas[i * (SIZE / ITERS)];
		strncpy(addr, MSG, sizeof(MSG));
		kick_reader(writefd);
	}
}

void
cull_reader(pid_t pid)
{
	int status;

	if (pid != 0) {
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status)) {
			fprintf(stderr, "reader %d did not exit", pid);
			exit(1);
		}

		if (WEXITSTATUS(status) != 0) {
			fprintf(stderr, "reader nonzero status %d",
			    WEXITSTATUS(status));
			exit(1);
		}
	}
}

void
test_round(size_t rounds)
{
	int pipes[2];
	int error;
	pid_t pid;
	void *sas;
	int i;

	sas = mmap(NULL, SIZE, PROT_READ | PROT_WRITE,
	    MAP_ANONYMOUS | MAP_SHARED, -1, 0);
	if (sas == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	minherit(sas, SIZE, INHERIT_SHARE);

	error = pipe(pipes);
	if (error != 0) {
		perror("pipes");
		exit(1);
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	for (i = 0; i < rounds; i++)
		pid == 0 ? read_mapping(sas, pipes[0]) :
			   write_mapping(sas, pipes[1]);

	pid == 0 ? exit(0) : cull_reader(pid);
}

int
main(int argc, char *argv[])
{
	size_t iters, rounds;
	int error;
	int i;

	if (argc < 3) {
		printf("Usage: ./sas <path> <# of files>\n");
		exit(1);
	}

	rounds = strtol(argv[2], NULL, 10);
	if (iters == 0)
		exit(1);

	iters = strtol(argv[3], NULL, 10);
	if (iters == 0)
		exit(1);

	for (i = 0; i < rounds; i++) {
		test_round(iters);
	}

	return (0);
}
