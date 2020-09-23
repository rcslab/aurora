#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sls.h>

static struct option spawn_longopts[] = {
	{ "oid", required_argument, NULL, 'o' },
	{ NULL, no_argument, NULL, 0 },
};

void
spawn_usage(void)
{
	printf("Usage: slsctl spawn -o <id> -- <command> [<arg> [<arg> ...]]\n");
}

int
spawn_main(int argc, char* argv[])
{
	int ret = EXIT_FAILURE;
	int oid_set;
	uint64_t oid;
	int opt;
	const char *cmd;
	pid_t pid;
	int pipefd[2];
	int attached;
	int wstatus;

	oid = 0;
	oid_set = 0;

	while ((opt = getopt_long(argc, argv, "o:", spawn_longopts, NULL)) != -1) {
		switch (opt) {
		case 'o':
		if (oid_set == 1) {
			spawn_usage();
			goto out;
		}

		oid = strtol(optarg, NULL, 10);
		oid_set = 1;
		break;

		default:
			spawn_usage();
			goto out;
		}
	}

	if (oid_set == 0) {
		spawn_usage();
		goto out;
	}

	cmd = argv[optind];

	if (pipe2(pipefd, O_CLOEXEC) != 0) {
		perror("pipe2()");
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork()");
		goto out;
	} else if (pid == 0) {
		close(pipefd[1]);

		// Wait for the parent to attach before execing
		if (read(pipefd[0], &attached, sizeof(attached)) == sizeof(attached) && attached) {
			execvp(cmd, argv + optind);
			perror("execvp()");
		}

		_Exit(EXIT_FAILURE);
	} else {
		close(pipefd[0]);

		if (sls_attach(oid, pid) < 0) {
			perror("sls_attach()");
			goto out;
		}

		attached = 1;
		if (write(pipefd[1], &attached, sizeof(attached)) != sizeof(attached)) {
			perror("write()");
			goto out;
		}

		close(pipefd[1]);

		waitpid(pid, &wstatus, 0);
		if (WIFEXITED(wstatus)) {
			ret = WEXITSTATUS(wstatus);
			if (ret != EXIT_SUCCESS) {
				fprintf(stderr, "%s exited with status %d\n", cmd, ret);
			}
		} else if (WIFSIGNALED(wstatus)) {
			psignal(WTERMSIG(wstatus), cmd);
		}
	}

out:
	exit(ret);
}
