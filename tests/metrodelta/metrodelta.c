#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <sls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#define LOCALHOST ("127.0.0.1")
#define SOCKET (7779)
#define BACKLOG (512)

#define BUF_SIZE (1024)
#define MSG ("Hey there")
#define MSGLEN (sizeof(MSG))

char buf[BUF_SIZE];

static struct option longopts[] = {
	{ "oid", required_argument, NULL, 'o' },
	{ "socket", required_argument, NULL, 's' },
	{ NULL, no_argument, NULL, 0 },
};

void
usage(void)
{
	printf("Usage: ./metrodelta -s <socket> -o <oid>\n");
}

int
getmessage(int sock)
{
	struct sockaddr_in dataaddr;
	socklen_t addrlen;
	ssize_t datalen;
	int datasock;
	int error;

	addrlen = sizeof(dataaddr);
	datasock = accept(sock, (struct sockaddr *)&dataaddr, &addrlen);
	if (datasock == -1) {
		perror("accept");
		exit(EX_USAGE);
	}

	/* This is where we bounce back after restoring. */
	datalen = recv(datasock, buf, MSGLEN, 0);
	if (datalen == -1) {
		perror("recv");
		return (EX_USAGE);
	}

	if (strncmp(buf, MSG, sizeof(MSG))) {
		printf("Test fails, message is %s\n", buf);
		exit(EX_DATAERR);
	}

	close(datasock);

	return (0);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in listaddr;
	uint64_t oid = 0;
	socklen_t addrlen;
	int error;
	int sock;
	int opt;

	listaddr.sin_family = AF_INET;
	listaddr.sin_port = htons(SOCKET);

	while ((opt = getopt_long(argc, argv, "o:s:", longopts, NULL)) != -1) {
		switch (opt) {
		case 'o':
			oid = strtoull(optarg, NULL, 10);
			if (oid == 0) {
				usage();
				exit(EX_USAGE);
			}
			break;

		case 's':
			listaddr.sin_port = htons(strtol(optarg, NULL, 10));
			if (listaddr.sin_port == 0) {
				usage();
				exit(EX_USAGE);
			}
			break;

		default:
			printf("Invalid argument %s\n", optarg);
			usage();
			exit(EX_USAGE);
		}
	}

	if (oid == 0) {
		usage();
		exit(EX_USAGE);
	}

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		perror("socket");
		exit(EX_OSERR);
	}

	error = inet_pton(AF_INET, LOCALHOST, &listaddr.sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		exit(EX_USAGE);
	}

	opt = 1;
	error = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (error == -1) {
		perror("setsockopt");
		exit(EX_USAGE);
	}

	opt = 1;
	error = setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
	if (error == -1) {
		perror("setsockopt");
		exit(EX_USAGE);
	}

	error = bind(sock, (struct sockaddr *)&listaddr, sizeof(listaddr));
	if (error == -1) {
		perror("bind");
		exit(EX_USAGE);
	}

	error = listen(sock, BACKLOG);
	if (error == -1) {
		perror("listen");
		exit(EX_UNAVAILABLE);
	}

	error = sls_checkpoint(oid, true);
	if (error != 0)
		exit(EX_OSERR);

	/* Use the socket to send a message. */
	error = getmessage(sock);
	if (error != 0) {
		printf("Failed with %d", error);
		exit(EX_USAGE);
	}

	return (EX_OK);
}
