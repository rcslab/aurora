#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/sbuf.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <arpa/inet.h>
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

static struct option partadd_send_longopts[] = {
	{ "address", required_argument, NULL, 'A' },
	{ "delta", no_argument, NULL, 'd' },
	{ "ignore unlinked files", required_argument, NULL, 'i' },
	{ "oid", required_argument, NULL, 'o' },
	{ "port", required_argument, NULL, 'P' },
	{ "period", required_argument, NULL, 't' },
	{ NULL, no_argument, NULL, 0 },
};

void
partadd_send_usage(void)
{
	partadd_base_usage("send", partadd_send_longopts);
}

void
partadd_send_socket(uint64_t oid, char *addr, int port, int *fdp)
{
	struct sockaddr_in sa;
	int error;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		partadd_send_usage();
		exit(0);
	}

	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	error = inet_pton(AF_INET, optarg, &sa.sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		partadd_send_usage();
		exit(0);
	}

	error = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
	if (error < 0) {
		perror("connect");
		partadd_send_usage();
		exit(0);
	}

	*fdp = fd;
}

void
partadd_send_write(int fd, uint64_t oid)
{
	struct slsmsg_register *regmsg;
	union slsmsg msg;
	int ret;

	/* XXX + 1 due to a hack to collocate the server and the
	 * client. */
	regmsg = (struct slsmsg_register *)&msg;
	*regmsg = (struct slsmsg_register) {
		.slsmsg_type = SLSMSG_REGISTER,
		.slsmsg_oid = oid + 1,
	};

	ret = write(fd, &msg, sizeof(msg));
	if (ret < 0) {
		perror("write");
		exit(0);
	}
}

int
partadd_send_main(int argc, char *argv[])
{
	struct sls_attr attr;
	uint64_t oid = 0;
	char *addr = NULL;
	int port = 0;
	int error;
	int opt;
	int fd;

	attr = (struct sls_attr) {
		.attr_target = SLS_SOCKSND,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = 0,
		.attr_amplification = 1,
	};

	while ((opt = getopt_long(argc, argv,
		    "A:dio:P:t:", partadd_send_longopts, NULL)) != -1) {
		switch (opt) {
		case 'A':
			if (port == 0 || oid == 0) {
				fprintf(stderr,
				    "port and oid go before the address\n");
				return (0);
			}

			partadd_send_socket(oid, optarg, port, &fd);
			partadd_send_write(fd, oid);
			break;

		case 'd':
			attr.attr_mode = SLS_DELTA;
			break;

		case 'i':
			attr.attr_flags |= SLSATTR_IGNUNLINKED;
			break;

		case 'o':
			oid = strtol(optarg, NULL, 10);
			break;

		case 'P':
			port = strtol(optarg, NULL, 10);
			break;

		case 't':
			attr.attr_period = strtol(optarg, NULL, 10);
			break;

		default:
			printf("Invalid option '%c'\n", opt);
			partadd_send_usage();
			return (0);
		}
	}

	if (oid == 0 || optind != argc) {
		partadd_send_usage();
		return (0);
	}

	if (sls_partadd(oid, attr, fd) < 0)
		return (1);

	return (0);
}
