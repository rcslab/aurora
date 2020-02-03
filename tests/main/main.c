#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

#include <sls.h>

const uint64_t oid = 1;

static void
usage(void)
{
	printf("Usage: ./slstest <test name> <iterations> <period> [test arguments...]\n");
	exit(0);
}

static void *
malloc_safe(size_t size)
{
	void *p;
	
	p = malloc(size);
	if (p == NULL) {
	    fprintf(stderr, "malloc: out of memory\n");
	    exit(-1);
	}

	return (p);
}

static int
exec_child(char *path, int argc, char *argv[])
{
	char **newargv;
	size_t newargc;

	/* 
	 * In the new argument vector we don't have the first three arguments,
	 * but we do have an extra NULL at the end we have to account for.
	 */
	newargc = argc - 3;

	/* Give the path as the first argument. */
	newargv = malloc_safe(sizeof(*newargv) * (newargc + 1));
	newargv[0] = path;

	/* Copy the rest of the arguments into the array. */
	memcpy(&newargv[1], &argv[4], sizeof(*newargv) * (newargc - 1));

	/* The array is NULL terminated. */
	newargv[newargc] = NULL;

	execvp(path, newargv);

	/* If we're here execve() failed. */
	perror("execvp");
	exit(-1);
}

static int
checkpoint(pid_t pid, int period)
{
	int error;

	const struct sls_attr attr = (struct sls_attr) {
	    .attr_target = SLS_OSD,
	    .attr_mode = SLS_FULL,
	    .attr_period = period,
	};
	
	error = sls_partadd(oid, attr);
	if (error != 0) {
	    fprintf(stderr, "sls_partadd failed with %d\n", error);
	    return (error);
	}

	error = sls_attach(oid, pid);
	if (error != 0) {
	    fprintf(stderr, "sls_attach failed with %d\n", error);
	    return (error);
	}

	error = sls_checkpoint(oid, true);
	if (error != 0) {
	    fprintf(stderr, "sls_checkpoint failed with %d\n", error);
	    return (error);
	}

	return (0);
}

/*
 * Do one test cycle for the test. The cycle goes as follows:
 * - A child is forked from the parent, 
 *   and starts executing the test.
 * - The parent checkpoints the child and waits for it to finish.
 * - The parent then
 */
int
slstest(char *path, int period, int argc, char *argv[])
{
	int wstatus;
	int error;
	pid_t pid;


	pid = fork();
	if (pid < 0) {
	    perror("fork");
	    exit(-1);
	}

	if (pid == 0) {
	    /* Doesn't return. */
	    exec_child(path, argc, argv);

	} else {
	    sleep(5); 
	    printf("Checkpointing...\n");

	    error = checkpoint(pid, period);
	    if (error != 0) {
		goto error;
	    }

	    /* wait() call, check return value */
	    for (;;) {
		error = waitpid(pid, &wstatus, 0);
		if (error != pid) {
		    perror("wait");
		    goto error;
		}

		/* We only care if the child exited. */
		if (!WIFEXITED(wstatus))
		    continue;

		if (WEXITSTATUS(wstatus) != 0) {
		    fprintf(stderr, "ERROR: Child exited with %d\n", error);
		    goto error;
		}

		break;
	    }
	}
	printf("Restoring...\n");

	/* Do not detach the children, we need to wait for them. */
	error = sls_restore(oid, false);
	sls_partdel(oid);
	if (error != 0) {
	    fprintf(stderr, "ERROR: sls_restore() exited with %d\n", error);
	    goto error;
	}

	for (;;) {
	    error = waitpid(pid, &wstatus, 0);
	    if (error != pid) {
		perror("wait");
		goto error;
	    }

	    /* We only care if the child exited. */
	    if (!WIFEXITED(wstatus))
		continue;

	    if (WEXITSTATUS(wstatus) != 0) {
		fprintf(stderr, "ERROR: Child exited with %d\n", error);
		goto error;
	    }

	    printf("Test %s successful.\n", path);
	}
	printf("Done!\n");

	return (0);

error:
	sls_partdel(oid);
	kill(pid, SIGKILL);

	exit(-1);
}

int
main(int argc, char *argv[])
{
	char *testname, *path;
	int iterations;
	size_t size;
	pid_t child;
	int period;
	int error;
	int i;

	/* Verify and unpack the arguments. */
	if (argc < 4)
	    usage();

	testname = argv[1];

	path = malloc_safe(PATH_MAX);

	/* 
	 * Construct the path. If the test has name n, the path has the form n/n 
	 * if we assume that we execute in the tests directory. 
	 */
	size = strlcpy(path, "/root/sls/tests/", PATH_MAX);
	if (size < strnlen(testname, PATH_MAX)) {
	   fprintf(stderr, "strlcat: path too large\n");
	   exit(-1);
	}

	size = strlcat(path, testname, PATH_MAX);
	if (size < strnlen(testname, PATH_MAX)) {
	   fprintf(stderr, "strlcat: path too large\n");
	   exit(-1);
	}

	size = strlcat(path, "/", PATH_MAX);
	if (size < strnlen("/", PATH_MAX)) {
	   fprintf(stderr, "strlcat: path too large\n");
	   exit(-1);
	}

	size = strlcat(path, testname, PATH_MAX);
	if (size < strnlen(testname, PATH_MAX)) {
	   fprintf(stderr, "strlcat: path too large\n");
	   exit(-1);
	}
	printf("PATH: %s\n", path);

	iterations = strtol(argv[2], NULL, 10);
	if (iterations == 0)
	    usage();

	/* If the string is invalid, we return 0 and continue forward. */
	period = strtol(argv[3], NULL, 10);

	/* Continuously run the test. */
	for (i = 0; i < iterations; i++) {
	    error = slstest(path, period, argc, argv);
	    if (error != 0)
		exit(-1);
	}

	return (0);
}
