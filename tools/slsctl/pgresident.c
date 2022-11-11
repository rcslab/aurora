#include <sys/types.h>
#include <sys/ioctl.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static struct option pgresident_longopts[] = {
	{ "oid", no_argument, NULL, 'o' },
	{ "filename", required_argument, NULL, 'f' },
	{ NULL, no_argument, NULL, 0 },
};

void
pgresident_usage(void)
{
	printf("Usage: slsctl pgresident -o <oid> -f <filename>\n");
}

int
pgresident_main(int argc, char *argv[])
{
	char *filename = NULL;
	uint64_t oid = 0;
	int error;
	int opt;
	int fd;

	while ((opt = getopt_long(
		    argc, argv, "f:o:", pgresident_longopts, NULL)) != -1) {
		switch (opt) {
		case 'f':
			filename = optarg;
			break;

		case 'o':
			/* The ID of the process. */
			oid = strtol(optarg, NULL, 10);
			break;

		default:
			pgresident_usage();
			return (0);
		}
	}

	if (optind != argc) {
		pgresident_usage();
		return (0);
	}

	if (oid == 0) {
		pgresident_usage();
		return (0);
	}

	if (filename == NULL) {
		pgresident_usage();
		return (0);
	}

	/* Open the file and pass it to the SLS for writing. */
	fd = open(filename, O_CREAT | O_EXCL | O_RDWR, 0644);
	if (fd < 0) {
		perror("open");
		return (1);
	}

	if ((error = sls_pgresident(oid, fd)) < 0)
		return (1);

	close(fd);

	return (0);
}
