#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <slos.h>
#include <sls.h>
#include <slsfs.h>
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
create_file(char *name)
{
	int error;

	error = slsfs_sas_create(name, SIZE);
	if (error != 0) {
		printf("slsfs_sas_map failed (error %d)\n", error);
		exit(1);
	}
}

/* Create the mapping, consume the file descriptor. */
void
map_file(int fd, void **sas)
{
	int error;

	error = slsfs_sas_map(fd, sas);
	if (error != 0) {
		printf("slsfs_sas_map failed\n");
		exit(1);
	}

	close(fd);
}

void
read_file(char *name, int readfd)
{
	char buf[sizeof(MSG)];
	void *sas, *addr;
	int fd, i;

	waitfor_writer(readfd);

	fd = open(name, O_RDWR, 0666);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	map_file(fd, &sas);
	printf("Reader found %p\n", sas);

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
write_file(char *name, int writefd)
{
	void *sas, *addr;
	int fd;
	int i;

	create_file(name);
	kick_reader(writefd);

	fd = open(name, O_RDWR, 0666);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	map_file(fd, &sas);
	printf("Writer found %p\n", sas);

	for (i = 0; i < ITERS; i++) {
		addr = &sas[i * (SIZE / ITERS)];
		strncpy(addr, MSG, sizeof(MSG));
		kick_reader(writefd);
	}
}

int
main(int argc, char *argv[])
{
	char fullname[PATH_MAX];
	char fileid[PATH_MAX];
	size_t numfiles;
	int pipes[2];
	int status;
	int error;
	pid_t pid;
	int i;

	if (argc < 3) {
		printf("Usage: ./sas <path> <# of files>\n");
		exit(1);
	}

	numfiles = strtol(argv[2], NULL, 10);
	if (numfiles == 0)
		exit(1);

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

	for (i = 0; i < numfiles; i++) {
		snprintf(fileid, PATH_MAX, "%06d", i);

		memset(fullname, 0, PATH_MAX);
		strncpy(fullname, argv[1], strnlen(argv[1], PATH_MAX));
		strncat(fullname, PATH, strnlen(PATH, PATH_MAX));
		strncat(fullname, fileid, strnlen(fileid, PATH_MAX));

		if (pid == 0)
			read_file(fullname, pipes[0]);
		else
			write_file(fullname, pipes[1]);
	}

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

	return (0);
}
