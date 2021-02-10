#include <sys/param.h>
#include <sys/mman.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	void *addr;
	int error;

	printf("1\n");
	/* Attach ourselves to the partition. */
	error = sls_attach(SLS_DEFAULT_MPARTITION, getpid());
	if (error != 0)
		exit(1);

	printf("2\n");
	/* Do a full checkpoint. */
	error = sls_checkpoint(SLS_DEFAULT_MPARTITION, false);
	if (error != 0)
		exit(1);

	printf("3\n");
	/* If we restore successfully, sleep for a bit and exit. */
	sleep(2);

	return (0);
}
