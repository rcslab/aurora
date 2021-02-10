#include <sys/types.h>
#include <sys/socket.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOCALHOST ("127.0.0.1")
#define SOCKET (6668)
#define BACKLOG (4)
#define MSG ("message")
#define BUFSIZE (sizeof(MSG))

char buf[BUFSIZE];

int
getmessage(int listsock)
{
	struct sockaddr_in dataaddr;
	socklen_t addrlen;
	ssize_t datalen;
	int datasock;
	int error;

	printf("Waiting for a connection...\n");
	addrlen = sizeof(dataaddr);
	datasock = accept(listsock, (struct sockaddr *)&dataaddr, &addrlen);
	if (datasock == -1) {
		perror("accept");
	}

	printf("Connection established.\n");
	printf("Socket fd: %d\n", datasock);
	printf("Remote Address: %s:%d\n", inet_ntoa(dataaddr.sin_addr),
	    ntohs(dataaddr.sin_port));

	for (;;) {
		/* Cleanup the buffer on every message. */
		memset(buf, 0, BUFSIZE);

		datalen = recv(datasock, buf, BUFSIZE, 0);
		if (datalen == -1) {
			perror("recv");
			/* This is where we bounce back after restoring. */
			return (0);
		}

		if (datalen == 0)
			break;

		printf("Data received: %s\n", buf);
	}

	printf("Connection done.\n");

	close(datasock);
	return (0);
}

int
main(void)
{
	struct sockaddr_in listaddr;
	socklen_t addrlen;
	int listsock;
	int option;
	int error;

	listsock = socket(AF_INET, SOCK_STREAM, 0);
	if (listsock == -1) {
		perror("socket");
		exit(0);
	}

	listaddr.sin_family = AF_INET;
	listaddr.sin_port = htons(SOCKET);
	error = inet_pton(AF_INET, LOCALHOST, &listaddr.sin_addr.s_addr);
	if (error == -1) {
		perror("inet_pton");
		exit(0);
	}

	option = 1;
	error = setsockopt(listsock, SOL_SOCKET, (SO_REUSEPORT | SO_REUSEADDR),
	    &option, sizeof(option));

	error = bind(listsock, (struct sockaddr *)&listaddr, sizeof(listaddr));
	if (error == -1) {
		perror("bind");
		exit(0);
	}

	error = listen(listsock, BACKLOG);
	if (error == -1) {
		perror("listen");
		exit(0);
	}

	/* Pre-restore setup. Open one connection, get data, and stay open.  */
	error = getmessage(listsock);
	if (error != 0) {
		printf("Failed with %d", error);
		exit(1);
	}

	/* Post-restore. Recovered from the killed connection, now get data. */
	error = getmessage(listsock);
	if (error != 0) {
		printf("Failed with %d", error);
		exit(1);
	}

	return (0);
}
