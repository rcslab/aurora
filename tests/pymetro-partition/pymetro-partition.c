#include <sys/param.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#define OID (1000)
#define LOCALHOST ("127.0.0.1")
#define PARENTSOCK (7776)
#define PARENTSOCKSTR ("7776")
#define SOCKET (7777)
#define SCALING (5)
#define BACKLOG (4)
#define REPEATS (4)
#define BUFSIZE (128)
char buf[BUFSIZE];

static struct option pymetro_longopts[] = {
	{ "accept4", no_argument, NULL, '4' },
	{ "cached", no_argument, NULL, 'c' },
	{ NULL, no_argument, NULL, 0 },
};

/* Test whether we're in Metropolis mode. */
void
testsls(bool expected)
{
	uint64_t realoid;
	bool insls;
	int error;

	error = sls_insls(&realoid, &insls);
	if (error != 0) {
		perror("sls_insls");
		exit(EX_OSERR);
	}

	if (insls != expected) {
		if (!insls)
			fprintf(stderr,
			    "Metropolis process %d not in the SLS\n", getpid());
		else
			fprintf(stderr, "Parent process %d is in the SLS\n",
			    getpid());
		exit(EX_OSERR);
	}
}

void
wait_child(void)
{
	int error;
	int wstatus;
	int status;

	error = wait(&wstatus);
	if (error < 0) {
		perror("wait");
		exit(EX_SOFTWARE);
	}

	if (!WIFEXITED(wstatus)) {
		fprintf(stderr, "Abnormal child exit\n");
		exit(EX_SOFTWARE);
	}

	status = WEXITSTATUS(wstatus);
	if (status != 0) {
		fprintf(stderr, "Child failed to exit normally (status %d)\n",
		    status);
		exit(EX_SOFTWARE);
	}
}

/*
 * Set up the listening socket address.
 */
void
testaccept_sockaddr(struct sockaddr_in *sa)
{
	int error;

	sa->sin_family = AF_INET;
	sa->sin_port = htons(PARENTSOCK);
	error = inet_pton(AF_INET, LOCALHOST, &sa->sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		exit(EX_USAGE);
	}
}

void
testaccept_listening(struct sockaddr_in *sa, int *fd)
{
	socklen_t addrlen;
	int listsock;
	int option;
	int error;

	/*  Set up the listening socket. */
	listsock = socket(AF_INET, SOCK_STREAM, 0);
	if (listsock == -1) {
		perror("socket");
		exit(EX_OSERR);
	}

	option = 1;
	error = setsockopt(
	    listsock, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
	if (error == -1) {
		perror("setsockopt");
		exit(EX_OSERR);
	}

	option = 1;
	error = setsockopt(
	    listsock, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	if (error == -1) {
		perror("setsockopt");
		exit(EX_OSERR);
	}

	error = bind(listsock, (struct sockaddr *)sa, sizeof(*sa));
	if (error == -1) {
		perror("bind");
		exit(EX_OSERR);
	}

	error = listen(listsock, BACKLOG);
	if (error == -1) {
		perror("listen");
		exit(EX_OSERR);
	}

	*fd = listsock;
}

/*
 * Set up a listening socket, then spawn a child that connects. For every
 * incoming connection, trigger Metropolis and hand off the socket to the new
 * instance.
 */
void
testaccept_parent(void)
{
	char *args[] = { "python3", "pymetro/client.py", PARENTSOCKSTR, NULL };
	struct sockaddr_in sa;
	int listsock, datasock;
	int error, i, j;
	pid_t pid;

	/* Wait for the children to exit due to checkpoint themselves. */
	for (i = 0; i < SCALING; i++)
		wait_child();

	/* The child is done, set up the listening socket for ourselves. */
	testaccept_sockaddr(&sa);
	testaccept_listening(&sa, &listsock);

	/* Create a new socket. We will connect to a new instance. */
	for (i = 0; i < REPEATS; i++) {
		for (j = 0; j < SCALING; j++) {
			/* Create the clients. */
			pid = fork();
			if (pid < 0) {
				perror("fork");
				exit(EX_OSERR);
			}

			if (pid == 0) {
				error = execve(
				    "/usr/local/bin/python3", args, NULL);
				if (error < 0) {
					perror("execve");
					exit(EX_OSERR);
				}
			}
		}

		/* Create as many forks as we need. */
		for (j = 0; j < SCALING; j++) {
		retry:
			error = sls_metropolis_spawn(OID + i, listsock);
			if (error == EAGAIN) {
				printf(
				    "PID of function already in use, retrying...\n");
				sleep(1);
				goto retry;
			}

			if (error != 0) {
				perror("sls_metropolis_spawn");
				exit(EX_OSERR);
			}
		}

		/* Wait for all children, clients and servers. */
		for (j = 0; j < SCALING; j++) {
			wait_child();
			wait_child();
		}
	}

	exit(EX_OK);
}

int
main(int argc, char **argv)
{
	char *args[] = { "python3", "pymetro/server.py", buf, NULL };
	struct sls_attr attr;
	bool cached = false;
	bool isaccept4 = false;
	uint64_t oid;
	pid_t pid;
	int error;
	long opt;
	int i;

	while ((opt = getopt_long(argc, argv, "4c", pymetro_longopts, NULL)) !=
	    -1) {
		switch (opt) {
		case '4':
			isaccept4 = true;
			break;

		case 'c':
			cached = true;
			break;

		default:
			printf("Usage:./pymetro [-4]\n");
			exit(EX_USAGE);
			break;
		}
	}

	/* Create independent server processes. */
	for (i = 0; i < SCALING; i++) {
		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(EX_OSERR);
		}

		if (pid == 0) {
			/* Create the private partition for the child. */
			oid = OID + i;
			attr = (struct sls_attr) {
				.attr_target = SLS_OSD,
				.attr_mode = SLS_DELTA,
				.attr_flags = SLSATTR_IGNUNLINKED |
				    SLSATTR_LAZYREST,
			};

			if (cached)
				attr.attr_flags |= SLSATTR_CACHEREST;

			error = sls_partadd(oid, attr);
			if (error != 0) {
				perror("sls_partadd");
				exit(EX_OSERR);
			}

			/* Enter Metropolis mode using that partition. */
			error = sls_metropolis(oid);
			if (error != 0) {
				perror("sls_metropolis");
				exit(EX_OSERR);
			}

			/* Each child gets their own litening socket port. */
			snprintf(buf, BUFSIZE, "%d", SOCKET + i);

			/* Call the server. */
			error = execve("/usr/local/bin/python3", args, NULL);
			if (error != 0)
				perror("execve");

			exit(EX_OSERR);
		}
	}

	/* The parent isn't in Metropolis. */
	testsls(false);

	testaccept_parent();

	return (0);
}
