#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <netinet/udp.h>

#define LOCALHOST   ("127.0.0.1")
#define SOCKET	    (6668)
#define BACKLOG	    (4)
#define MSG	    ("message")
#define BUFSIZE	    (sizeof(MSG))

char buf[BUFSIZE];

int
main(void)
{
	struct sockaddr_in sockaddr;
	ssize_t datalen;
	int sock;
	size_t len;
	int error;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == -1) {
	    perror("socket");
	    exit(0);
	}

	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(SOCKET);
	error = inet_pton(AF_INET, LOCALHOST, &sockaddr.sin_addr.s_addr);
	if (error == -1) {
	    perror("inet_pton");
	    exit(0);

	}

	error = bind(sock, (struct sockaddr *) &sockaddr, sizeof(sockaddr));
	if (error == -1) {
	    perror("bind");
	    exit(0);
	}

	sleep(5);
	memset(buf, 0, BUFSIZE);
	datalen = recv(sock, buf, BUFSIZE, 0);
	if (datalen == -1) {
		perror("recv");
		exit(1);
	}

	if (strncmp(buf, MSG, BUFSIZE)) {
		printf("Got %s instead of %s\n", buf, MSG);
		exit(1);
	}

	printf("Data received: %s\n", buf);
	printf("Server done.\n");

	return (0);
}
