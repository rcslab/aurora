#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <netinet/in.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define UNADDR ("localsocket")
/* Found by inspecting struct sockaddr_un. */
#define UNADDR_MAX (108)
#define BUFSIZE (1024)
#define PORT (6668)

char buf[BUFSIZE];

int
main(void)
{
	struct sockaddr_un local, remote;
	int lstsock, rcvsock;
	socklen_t addrlen;
	ssize_t msglen;
	int error;

	local.sun_family = AF_UNIX;
	strlcpy(local.sun_path, UNADDR, UNADDR_MAX);

	/* Create the blank socket. */
	lstsock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lstsock == -1) {
		perror("socket");
		exit(0);
	}

	/* Bind it to the address given in sockaddr_in. */
	error = bind(lstsock, (struct sockaddr *)&local, sizeof(local));
	if (error == -1) {
		perror("bind");
		exit(0);
	}

	error = listen(lstsock, 512);
	if (error == -1) {
		perror("listen");
		exit(0);
	}

	for (;;) {
		printf("Waiting for a connection...\n");

		addrlen = sizeof(remote);
		rcvsock = accept(lstsock, (struct sockaddr *)&remote, &addrlen);
		if (rcvsock < 0) {
			perror("accept");
			exit(0);
		}
		printf("Connection received.\n");

		for (;;) {
			msglen = recv(rcvsock, &buf, BUFSIZE, 0);
			if (msglen < 0) {
				perror("recv");
				exit(0);
			}

			if (msglen == 0)
				break;

			printf("Received: %s\n", buf);
		}

		printf("Connection done.\n");
	}

	exit(0);
}
