#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#define PORT (5040)
#define BACKLOG (512)
#define SLSNETROOT ("/slsnet")

#include <slos_inode.h>
#include <sls.h>
#include <sls_ioctl.h>
#include <sls_message.h>

/* SLS checkpoint server protocol. */

/*
 * XXX Add to the protocol to support bouncing
 * back from a replica/communication failure.
 */
/*
 * SLS server state.
 */

struct slsmsg_server {
	int slssrv_listsock;
	int slssrv_connsock;

	int slssrv_rootfd;
	int slssrv_epochfd;
	int slssrv_recfd;

	uint64_t slssrv_curoid;
	char *slssrv_recmapping;
	size_t slssrv_maplen;
	char *slssrv_root;
	char slssrv_buf[PATH_MAX + 1];
};

void
slssrv_init(struct slsmsg_server *srv, char *root)
{
	memset(srv, 0, sizeof(*srv));

	*srv = (struct slsmsg_server) {
		.slssrv_listsock = -1,
		.slssrv_connsock = -1,
		.slssrv_rootfd = -1,
		.slssrv_epochfd = -1,
		.slssrv_recfd = -1,
		.slssrv_root = root,
	};
}

void
slssrv_fini(struct slsmsg_server *srv)
{
	if (srv->slssrv_listsock >= 0)
		close(srv->slssrv_listsock);
	if (srv->slssrv_connsock >= 0)
		close(srv->slssrv_connsock);
	if (srv->slssrv_rootfd >= 0)
		close(srv->slssrv_rootfd);
	if (srv->slssrv_epochfd >= 0)
		close(srv->slssrv_epochfd);
	if (srv->slssrv_recfd >= 0)
		close(srv->slssrv_recfd);

	memset(srv, 0, sizeof(*srv));
}

void
slssrv_listen(struct slsmsg_server *srv, int port)
{
	struct sockaddr_in inaddr;
	int option;
	int error;
	int fd;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		perror("socket");
		exit(EX_OK);
	}

	inaddr.sin_family = AF_INET;
	inaddr.sin_port = htons(port);
	inaddr.sin_addr.s_addr = INADDR_ANY;

	option = 1;
	error = setsockopt(
	    fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
	if (error == -1) {
		perror("setsockopt");
		exit(EX_USAGE);
	}

	option = 1;
	error = setsockopt(
	    fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option));
	if (error == -1) {
		perror("setsockopt");
		exit(EX_USAGE);
	}

	error = bind(fd, (struct sockaddr *)&inaddr, sizeof(inaddr));
	if (error == -1) {
		perror("bind");
		exit(EX_USAGE);
	}

	error = listen(fd, BACKLOG);
	if (error == -1) {
		perror("listen");
		exit(EX_UNAVAILABLE);
	}

	srv->slssrv_listsock = fd;
}

void
slssrv_connect(struct slsmsg_server *srv)
{
	struct sockaddr_in inaddr;
	socklen_t addrlen;
	int error;
	int fd;

	printf("Waiting for a connection...\n");
	addrlen = sizeof(inaddr);
	fd = accept(srv->slssrv_listsock, (struct sockaddr *)&inaddr, &addrlen);
	if (fd == -1) {
		perror("accept");
		exit(EX_USAGE);
	}

	printf("Connection established. ");
	printf("Remote Address: %s:%d\n", inet_ntoa(inaddr.sin_addr),
	    ntohs(inaddr.sin_port));

	srv->slssrv_connsock = fd;
}

int
slssrv_register(struct slsmsg_server *srv, union slsmsg *msg)
{
	struct slsmsg_register regmsg = *(struct slsmsg_register *)msg;
	ssize_t remaining, received;
	struct sls_attr attr;
	int error;
	int fd;

	memset(&attr, 0, sizeof(attr));
	attr = (struct sls_attr) {
		.attr_target = SLS_FILE,
		.attr_mode = SLS_FULL,
		.attr_period = 0,
		.attr_flags = SLSATTR_IGNUNLINKED | SLSATTR_NOCKPT,
		.attr_amplification = 1,
	};

	snprintf(srv->slssrv_buf, PATH_MAX, "%s/%ld", srv->slssrv_root,
	    regmsg.slsmsg_oid);
	error = mkdir(srv->slssrv_buf, 0660);
	if (error != 0 && errno != EEXIST) {
		perror("mkdir");
		exit(EX_CANTCREAT);
	}

	fd = open(srv->slssrv_buf, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		perror("open");
		exit(EX_OSFILE);
	}

	printf("Creating %lx\n", regmsg.slsmsg_oid);
	sls_partadd(regmsg.slsmsg_oid, attr, fd);

	srv->slssrv_rootfd = fd;

	return (0);
}

int
slssrv_setupckpt(struct slsmsg_server *srv, union slsmsg *msg)
{
	struct slsmsg_ckptstart *setupmsg = (struct slsmsg_ckptstart *)msg;
	int error;
	int fd;

	snprintf(srv->slssrv_buf, PATH_MAX, "%ld", setupmsg->slsmsg_epoch);
	error = mkdirat(srv->slssrv_rootfd, srv->slssrv_buf, 0660);
	if (error != 0) {
		perror("mkdirat");
		exit(EX_CANTCREAT);
	}

	fd = openat(
	    srv->slssrv_rootfd, srv->slssrv_buf, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		perror("open");
		exit(EX_OSFILE);
	}

	srv->slssrv_epochfd = fd;

	return (0);
}

int
slssrv_storemeta(struct slsmsg_server *srv, union slsmsg *msg)
{
	struct slsmsg_recmeta *metamsg = (struct slsmsg_recmeta *)msg;
	ssize_t received;
	void *addr;
	int error;
	int fd;

	if (srv->slssrv_recfd >= 0) {
		munmap(srv->slssrv_recmapping, srv->slssrv_maplen);
		close(srv->slssrv_recfd);

		srv->slssrv_recfd = -1;
		srv->slssrv_recmapping = NULL;
		srv->slssrv_maplen = 0;
	}

	snprintf(srv->slssrv_buf, PATH_MAX, "%ld", metamsg->slsmsg_uuid);
	printf("Opening %s\n", srv->slssrv_buf);
	fd = openat(
	    srv->slssrv_epochfd, srv->slssrv_buf, O_RDWR | O_CREAT | O_TRUNC);
	if (fd < 0) {
		perror("open");
		exit(EX_OSFILE);
	}

	/* For compatibility reasons with the file write path. */
	if (metamsg->slsmsg_rectype == SLOSREC_MANIFEST)
		assert(metamsg->slsmsg_totalsize == metamsg->slsmsg_metalen);

	error = ftruncate(fd, metamsg->slsmsg_totalsize);
	if (error != 0) {
		perror("ftruncate");
		exit(EX_OSFILE);
	}

	addr = mmap(NULL, metamsg->slsmsg_totalsize, PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	if (addr == NULL) {
		perror("mmap");
		exit(EX_OSERR);
	}

	/* Again, for compatibility reasons. */
	received = recv(
	    srv->slssrv_connsock, addr, metamsg->slsmsg_metalen, MSG_WAITALL);
	if (received < 0) {
		perror("recv");
		exit(EX_PROTOCOL);
	}

	assert(received == metamsg->slsmsg_metalen);

	if (metamsg->slsmsg_rectype != SLOSREC_VMOBJ) {
		munmap(addr, metamsg->slsmsg_totalsize);
		error = ftruncate(fd, metamsg->slsmsg_totalsize);
		if (error != 0) {
			perror("ftruncate");
			exit(EX_OSFILE);
		}
		close(fd);
	} else {
		srv->slssrv_recfd = fd;
		srv->slssrv_curoid = metamsg->slsmsg_uuid;
		srv->slssrv_recmapping = addr;
		srv->slssrv_maplen = metamsg->slsmsg_totalsize;
	}

	return (0);
}

int
slssrv_storepages(struct slsmsg_server *srv, union slsmsg *msg)
{
	struct slsmsg_recpages *pagemsg = (struct slsmsg_recpages *)msg;
	size_t offset = pagemsg->slsmsg_offset;
	size_t received, txsize;
	char *buf;

	assert(srv->slssrv_recfd >= 0);

	buf = &srv->slssrv_recmapping[offset];
	txsize = pagemsg->slsmsg_len;

	received = recv(srv->slssrv_connsock, buf, txsize, MSG_WAITALL);
	if (received < 0) {
		perror("recv");
		exit(EX_PROTOCOL);
	}

	assert(received == txsize);

	return (0);
}

int
slssrv_teardownckpt(struct slsmsg_server *srv, union slsmsg *msg)
{
	struct slsmsg_ckptdone *tearmsg = (struct slsmsg_ckptdone *)msg;
	size_t received;

	if (srv->slssrv_recfd >= 0) {
		munmap(srv->slssrv_recmapping, srv->slssrv_maplen);
		srv->slssrv_recmapping = NULL;
		srv->slssrv_maplen = 0;

		close(srv->slssrv_epochfd);
		srv->slssrv_recfd = -1;
	}

	if (srv->slssrv_epochfd >= 0) {
		close(srv->slssrv_epochfd);
		srv->slssrv_epochfd = -1;
	}

	return (0);
}

int
slssrv_recvmsg(struct slsmsg_server *srv, bool *donep)
{
	enum slsmsgtype msgtype;
	ssize_t received;
	union slsmsg msg;
	int error;

	received = recv(srv->slssrv_connsock, &msg, sizeof(msg), MSG_WAITALL);
	if (received < 0) {
		perror("recv");
		exit(EX_PROTOCOL);
	}

	assert(received == sizeof(msg));

	/* The first member of all message structs is their type. */
	msgtype = *(enum slsmsgtype *)&msg;

	/*
	 * If we are doing a checkpoint operation we must
	 * already have a directory for the partition.
	 */
	if ((msgtype != SLSMSG_REGISTER) && (msgtype != SLSMSG_DONE))
		assert(srv->slssrv_rootfd != -1);

	switch (msgtype) {
	case SLSMSG_REGISTER:
		error = slssrv_register(srv, &msg);
		break;

	case SLSMSG_CKPTSTART:
		error = slssrv_setupckpt(srv, &msg);
		break;

	case SLSMSG_RECMETA:
		error = slssrv_storemeta(srv, &msg);
		break;

	case SLSMSG_RECPAGES:
		error = slssrv_storepages(srv, &msg);
		break;

	case SLSMSG_CKPTDONE:
		error = slssrv_teardownckpt(srv, &msg);
		/* XXX Temporary till we flesh out the protocol. */
		*donep = 0;
		break;

	case SLSMSG_DONE:
		*donep = true;
		error = 0;
		break;

	default:
		fprintf(stderr, ("invalid message type %d"), msgtype);
		exit(EX_PROTOCOL);
	}

	return (error);
}

int
main(int argc, char **argv)
{
	struct slsmsg_server srv;
	char *root;
	bool done;
	int error;

	if (argc != 2) {
		printf("usage: ./server <basedir>");
		exit(EX_USAGE);
	}
	root = argv[1];

	error = chdir(root);
	if (error != 0) {
		perror("chdir");
		exit(EX_USAGE);
	}

	slssrv_init(&srv, root);
	slssrv_listen(&srv, PORT);
	slssrv_connect(&srv);

	do {
		error = slssrv_recvmsg(&srv, &done);
		if (error != 0)
			break;
	} while (!done);

	slssrv_fini(&srv);

	return (0);
}
