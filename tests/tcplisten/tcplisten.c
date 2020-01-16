#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#define LOCALHOST   ("127.0.0.1")
#define SOCKET	    (6668)
#define BACKLOG	    (4)
#define BUFSIZE	    (1024)
#define MSGSIZE	    (4)

char buf[BUFSIZE];

int 
main(void)
{
	struct sockaddr_in listaddr, dataaddr;
	int listsock, datasock;
	socklen_t addrlen; 
	ssize_t datalen;
	size_t len;
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

	error = bind(listsock, (struct sockaddr *) &listaddr, sizeof(listaddr));
	if (error == -1) {
	    perror("bind");
	    exit(0);
	}

	error = listen(listsock, BACKLOG);
	if (error == -1) {
	    perror("listen");
	    exit(0);
	}

	for(;;) {
	    printf("Garbage fd: %d\n", datasock);
	    printf("Garbage Address: %s:%d\n", inet_ntoa(dataaddr.sin_addr), ntohs(dataaddr.sin_port));

	    printf("Waiting for a connection...\n");
	    addrlen = sizeof(dataaddr);
	    datasock = accept(listsock, (struct sockaddr *) &dataaddr, &addrlen);
	    if (datasock == -1) {
		perror("accept");
		exit(0);
	    }

	    printf("Connection established.\n");
	    printf("Socket fd: %d\n", datasock);
	    printf("Remote Address: %s:%d\n", inet_ntoa(dataaddr.sin_addr), ntohs(dataaddr.sin_port));

	    for (;;) {
		/* Cleanup the buffer on every message. */
		memset(buf, 0, BUFSIZE);

		len = MSGSIZE;
		datalen = recv(datasock, buf, len, 0);
		if (datalen == - 1) {
		    perror("recv");
		    break;
		}

		if (datalen == 0)
		    break;

		printf("Data received: %s\n", buf);
	    }

	    printf("Connection done.\n");

	    close(datasock);
	}

	return 0;
}
