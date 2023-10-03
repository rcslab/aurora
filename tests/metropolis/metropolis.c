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

#define OID (SLS_DEFAULT_PARTITION)
#define LOCALHOST ("127.0.0.1")
#define SOCKET (7778)
#define BACKLOG (4)
#define REPEATS (1)
#define MSG ("ready")
char buf[sizeof(MSG)];

static struct option metropolis_longopts[] = {
	{ "accept4", no_argument, NULL, '4' },
	{ "accept", no_argument, NULL, 'a' },
	{ "execve", no_argument, NULL, 'e' },
	{ "fork", no_argument, NULL, 'f' },
	{ "exit", no_argument, NULL, 'x' },
	{ NULL, no_argument, NULL, 0 },
};

/* Test whether we're in Metropolis mode. */
void
testsls(uint64_t oid, bool expected)
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

	if (!insls)
		return;

	if (realoid != OID) {
		fprintf(
		    stderr, "Metropolis process is in the wrong partition\n");
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
		fprintf(stderr, "Child didn't exit\n");
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
	sa->sin_port = htons(SOCKET);
	error = inet_pton(AF_INET, LOCALHOST, &sa->sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		exit(EX_USAGE);
	}
}

/*
 * Notify of an event from the child.
 */
void
testaccept_notify(int writefd)
{
	int ret;

	/* Notify the parent that everything is ready. */
	ret = write(writefd, MSG, sizeof(MSG));
	if (ret < 0) {
		perror("write");
		exit(EX_IOERR);
	}

	if (ret != sizeof(MSG)) {
		fprintf(stderr, "Partial write to fd\n");
		exit(EX_IOERR);
	}
}

/*
 * Wait for an event in the parent.
 */
void
testaccept_waitfor(int readfd)
{
	int ret;

	/* Wait for the child to get ready. */
	ret = read(readfd, buf, sizeof(MSG));
	if (ret < 0) {
		perror("read");
		exit(EX_IOERR);
	}

	if (ret != sizeof(MSG)) {
		fprintf(stderr, "Partial read from fd\n");
		exit(EX_IOERR);
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
 * Test the overloaded accept() call with a child calling accept().
 */
void
testaccept_child(int writefd, struct sockaddr_in *sa, bool isaccept4)
{
	struct sockaddr dataaddr;
	bool flags = O_NONBLOCK;
	int listsock, datasock;
	socklen_t addrlen;
	void *mapping;
	int error;
	int ret;

	/* Create the listening socket. */
	testaccept_listening(sa, &listsock);

	/* Notify the parent that it needs to connect. */
	testaccept_notify(writefd);
	close(writefd);

	/* Trigger a Metropolis checkpoint by accepting. */
	datasock = -1;
	if (isaccept4) {
		datasock = accept4(listsock, (struct sockaddr *)&dataaddr,
		    &addrlen, SOCK_NONBLOCK);
		if (datasock == -1) {
			perror("accept");
			exit(EX_OSERR);
		}
	} else {
		datasock = accept(
		    listsock, (struct sockaddr *)&dataaddr, &addrlen);
		if (datasock == -1) {
			perror("accept");
			exit(EX_OSERR);
		}
	}

	/* Notify the parent to keep executing through the new socket. */
	testaccept_notify(datasock);

	exit(EX_OK);
}

static void
testaccept_connchild(struct sockaddr_in *sa)
{
	int datasock;
	int error;

	/* Connect to the parent's socket. */
	datasock = socket(AF_INET, SOCK_STREAM, 0);
	if (datasock < 0) {
		perror("socket");
		exit(EX_OSERR);
	}

	error = connect(datasock, (struct sockaddr *)sa, sizeof(*sa));
	if (error == -1) {
		perror("connect");
		exit(EX_OSERR);
	}

	/* The socket was handed to the other child, wait for it to ping us. */
	testaccept_waitfor(datasock);

	exit(EX_OK);
}

/*
 * Set up a listening socket, then spawn a child that connects. For every
 * incoming connection, trigger Metropolis and hand off the socket to the new
 * instance.
 */
void
testaccept_parent(int readfd, struct sockaddr_in *sa)
{
	int listsock, datasock;
	int error, i;
	pid_t pid;

	/* Wait for the child to exit due to checkpointing itself. */
	wait_child();

	/* The child is done, set up the listening socket for ourselves. */
	testaccept_listening(sa, &listsock);

	/* Create a new socket. We will connect to a new instance. */
	for (i = 0; i < REPEATS; i++) {
		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(EX_OSERR);
		}

		if (pid == 0)
			testaccept_connchild(sa);

		error = sls_metropolis_spawn(OID, listsock);
		if (error != 0) {
			perror("socket");
			exit(EX_OSERR);
		}

		/* Wait for both children */
		wait_child();
		wait_child();
	}

	exit(EX_OK);
}

void
testexit(void)
{
	int error;
	pid_t pid;

	/*
	 * Fork. Each process enters Metropolis independently, with
	 * one exiting immediately and testing the instrumented exit,
	 * while the other one should be killed during module teardown.
	 */

	pid = fork();
	if (pid < 0) {
		perror("pid");
		exit(EX_OSERR);
	}

	error = sls_metropolis(OID);
	if (error != 0) {
		perror("sls_metropolis");
		exit(EX_OSERR);
	}

	/* The exiting process. */
	if (pid == 0)
		exit(EX_OK);

	/* The stalling process. */
	for (;;)
		sleep(1);

	printf("Should never reach this");
	exit(EX_SOFTWARE);
}

int
main(int argc, char **argv)
{
	const char *args[] = { argv[0], NULL };
	bool testexec = false, testexeced = false, tesexit = false;
	bool testaccept = false, testfork = false;
	struct sockaddr_in sa;
	bool isaccept4;
	int pipefd[2];
	int error;
	pid_t pid;
	long opt;

	while ((opt = getopt_long(argc, argv, "4aefx", metropolis_longopts,
		    NULL)) != -1) {
		switch (opt) {
		case '4':
			testaccept = true;
			isaccept4 = true;
			break;

		case 'a':
			testaccept = true;
			isaccept4 = false;
			break;

		case 'e':
			testexec = true;
			break;

		case 'f':
			testfork = true;
			break;

		case 'x':
			testexit();
			/* NOTREACHED */
			exit(EX_SOFTWARE);
			break;

		default:
			printf("Usage:./metropolis [-ef]\n");
			exit(EX_USAGE);
			break;
		}
	}


	/* If testing exec(), use this binary but with different arguments*/
	if (testexec) {
		error = execve(argv[0], (char **)args, NULL);
		if (error != 0) {
			perror("execve");
			exit(EX_USAGE);
		}
	}

	/* Testing fork(), create a child and make sure it's also in Aurora. */
	if (testfork || testaccept) {
		error = pipe(pipefd);
		if (error != 0) {
			perror("pipe");
			exit(EX_OSERR);
		}

		pid = fork();
		if (pid < 0) {
			perror("fork");
			exit(EX_OSERR);
		}

	}

	/*
	 * We don't get into Metropolis from the child when using fork(), but
	 * when calling accept().
	 */
	testsls(OID, false);

	/*
	 * If we are testing accept(), have the child open a connection and
	 * signal to the parent when it's ready. The parent actually connects
	 * and triggers the checkpoint.
	 */
	if (testaccept) {
		testaccept_sockaddr(&sa);

		if (pid == 0) {
			error = sls_metropolis(OID);
			if (error != 0) {
				perror("sls_metropolis");
				exit(EX_OSERR);
			}

			close(pipefd[0]);
			testaccept_child(pipefd[1], &sa, isaccept4);
		} else {
			close(pipefd[1]);
			testaccept_parent(pipefd[0], &sa);
		}
	}

	if (testfork && pid != 0) {
		wait_child();
	}

	return (0);
}
