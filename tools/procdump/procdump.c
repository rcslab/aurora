#include "slsmm.h"

#include <sys/ioctl.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Value of the filename argument 
 * that causes dumping to memory instead
 */
#define MEMDESC ("memory")

int main(int argc, char* argv[]) {
	int pid;
	int slsmm_fd;
	int error;
	int mode;
	int type;
	int async;
	struct dump_param param;

	if (argc != 5) {
		printf("Usage: procdump <filename> <PID> <Mode> <Sync=0/Async=1>\n");
		return 0;
	}

	pid = strtol(argv[argc-3], &argv[argc-3], 10); 
	mode = strtol(argv[argc-2], &argv[argc-2], 10); 
	async = strtol(argv[argc-1], &argv[argc-1], 10); 
	type = strcmp(argv[1], MEMDESC) ? 
				SLSMM_FD_FILE : SLSMM_FD_MEM;

	slsmm_fd = open("/dev/slsmm", O_RDWR);
	if (!slsmm_fd) {
		printf("ERROR: SLS device file not opened\n");
		exit(1); 
	}



	param = (struct dump_param) { 
		.name = argv[1],
		.len = strnlen(argv[1], 1024),
		.pid = pid, 
		.fd_type = type,
		.dump_mode = mode,
	};

	if (async) {
	    ioctl(slsmm_fd, SLSMM_ASYNC_DUMP, &param);
	    int flushed;
	    do {
		sleep(1);
		ioctl(slsmm_fd, SLSMM_FLUSH_COUNT, &flushed);
		printf("request id: %d, current: %d\n", param.request_id, flushed);
	    } while (param.request_id != flushed);


	} else
	    ioctl(slsmm_fd, SLSMM_DUMP, &param);

	/*
	if (type == SLSMM_FD_MEM) 
	    printf("memory descriptor: %d\n", param.fd);
	*/

	close(slsmm_fd);

	return 0;
}
