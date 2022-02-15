#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
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
#define MAX_ATTEMPTS (10)

static struct option longopts[] = {
	{ "socket", required_argument, NULL, 's' },
	{ NULL, no_argument, NULL, 0 },
};

void
usage(void)
{
	printf("Usage: ./metroclient -s <socket>\n");
}

int
putmessage(int sock, struct sockaddr *addr)
{
	int error;
	int ret;
	int i;

	/* Retry the connection until we get it. */
	for (i = 0; i < MAX_ATTEMPTS; i++) {
		ret = connect(sock, addr, sizeof(struct sockaddr_in));
		if (ret == 0)
			break;

		sleep(1);
	}

	if (i == MAX_ATTEMPTS) {
		fprintf(stderr, "Client could not connect.\n");
		exit(EX_OSERR);
	}

	ret = send(sock, MSG, MSGLEN, 0);
	if (ret < 0) {
		perror("send");
		exit(EX_USAGE);
	}

	close(sock);

	return (0);
}

int
main(int argc, char **argv)
{
	struct sockaddr_in listaddr;
	socklen_t addrlen;
	int error;
	int sock;
	int opt;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1) {
		perror("socket");
		exit(EX_OSERR);
	}

	listaddr.sin_family = AF_INET;
	listaddr.sin_port = htons(SOCKET);

	while ((opt = getopt_long(argc, argv, "s:", longopts, NULL)) != -1) {
		switch (opt) {
		case 's':
			listaddr.sin_port = htons(strtol(optarg, NULL, 10));
			if (listaddr.sin_port == 0) {
				usage();
				exit(EX_USAGE);
			}
			break;

		default:
			usage();
			exit(EX_USAGE);
		}
	}

	error = inet_pton(AF_INET, LOCALHOST, &listaddr.sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		exit(EX_USAGE);
	}

	error = putmessage(sock, (struct sockaddr *)&listaddr);
	if (error != 0) {
		printf("Failed with %d", error);
		exit(EX_USAGE);
	}

	return (EX_OK);
}
