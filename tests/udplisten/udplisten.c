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
#define BUFSIZE	    (1024)
#define MSGSIZE	    (4)

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

	printf("Waiting for data...\n");
	for(;;) {
	    /* Cleanup the buffer on every message. */
	    memset(buf, 0, BUFSIZE);

	    len = MSGSIZE;
	    datalen = recv(sock, buf, len, 0);
	    if (datalen == - 1) {
		perror("recv");
		exit(0);
	    }

	    if (datalen == 0)
		break;

	    printf("Data received: %s\n", buf);
	}
	printf("Server done.\n");

	return 0;
}
