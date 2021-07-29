#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <errno.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define PATH ("/tmp/kqueue.test")
#define BUF_SIZE (1024)
#define MESSAGE ("MESSAGE")

char buf[BUF_SIZE];

void
cleanup_file(int fd)
{
	int error;

	error = close(fd);
	if (error != 0) {
		/* Keep going, it's the unlinking that's crucial. */
		perror("close");
	}

	/* Do not unlink the file, else the restored test won't find it. */
}

int
main(int argc, char *argv[])
{
	struct kevent kev;
	int error, ret;
	int fd;
	int kq;

	strncpy(buf, MESSAGE, sizeof(MESSAGE));

	/* Create the file to generate the events. */
	fd = open(PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	/* Create a kqueue and register it. */
	kq = kqueue();
	if (kq < 0) {
		perror("kqueue");
		goto error;
	}

	EV_SET(&kev, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0, 0);

	ret = kevent(kq, &kev, 1, NULL, 0, NULL);
	if (ret == -1) {
		printf("Error by kevent registration\n");
		goto error;
	}

	ret = write(fd, buf, sizeof(MESSAGE));
	if (ret != sizeof(MESSAGE)) {
		perror("write");
		printf("Wrote only %d bytes\n", ret);
		error = EINVAL;
		goto error;
	}

	sleep(3);

	ret = kevent(kq, NULL, 0, &kev, 1, NULL);
	if (ret == -1) {
		printf("Error by kevent registration\n");
		goto error;
	}

	if (ret != 1) {
		printf("Expected 1 event, found %d\n", ret);
		goto error;
	}

	/* Write again to ensure the file is still open. */
	ret = write(fd, buf, sizeof(MESSAGE));
	if (ret != sizeof(MESSAGE)) {
		perror("write");
		printf("Wrote only %d bytes\n", ret);
		error = EINVAL;
		goto error;
	}

	cleanup_file(fd);

	return (0);

error:

	cleanup_file(fd);
	exit(1);
}
