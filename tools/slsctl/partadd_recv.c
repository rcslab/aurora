#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sls.h>
#include <sls_message.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "partadd.h"

#define BACKLOG (512)

static struct option partadd_recv_longopts[] = {
	{ "address", required_argument, NULL, 'A' },
	{ "delta", required_argument, NULL, 'd' },
	{ "oid", required_argument, NULL, 'o' },
	{ "port", required_argument, NULL, 'P' },
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_recv_usage(void)
{
	partadd_base_usage("recv", partadd_recv_longopts);
}

void
partadd_recv_socket(uint64_t oid, char *addr, int port, int *fdp)
{
	struct sockaddr_in sa;
	int option;
	int error;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		partadd_recv_usage();
		exit(0);
	}

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	error = inet_pton(AF_INET, optarg, &sa.sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		partadd_recv_usage();
		exit(0);
	}

	error = setsockopt(
	    fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	if (error == -1) {
		perror("setsockopt");
		exit(0);
	}

	option = 1;
	error = setsockopt(
	    fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
	if (error == -1) {
		perror("setsockopt");
		exit(0);
	}

	error = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (error == -1) {
		perror("bind");
		exit(0);
	}

	error = listen(fd, BACKLOG);
	if (error == -1) {
		perror("listen");
		exit(0);
	}

	*fdp = fd;
}

int
partadd_recv_main(int argc, char *argv[])
{
	struct sls_attr attr;
	uint64_t oid = 0;
	char *addr = NULL;
	int port = 0;
	int error;
	int opt;
	int fd;

	attr = (struct sls_attr) {
		.attr_target = SLS_SOCKRCV,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = SLSATTR_NOCKPT,
		.attr_amplification = 1,
	};

	while ((opt = getopt_long(argc, argv, "A:do:P:", partadd_recv_longopts,
		    NULL)) != -1) {
		switch (opt) {
		case 'A':
			if (port == 0 || oid == 0) {
				fprintf(stderr,
				    "port and oid go before the address\n");
				return (0);
			}

			partadd_recv_socket(oid, optarg, port, &fd);
			break;

		case 'd':
			attr.attr_mode = SLS_DELTA;
			break;

		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;

		case 'P':
			port = strtol(optarg, NULL, 10);
			break;

		default:
			printf("Invalid option '%c'\n", opt);
			partadd_recv_usage();
			return (0);
		}
	}

	if (oid == 0 || optind != argc) {
		partadd_recv_usage();
		return (0);
	}

	if (sls_partadd(oid, attr, fd) < 0)
		return (1);

	return (0);
}
