#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "slsmm.h"

int arr[128];

void full_dump_self(int sls) {
	struct dump_param param = {
	    .pid = getpid(),
	    .fd = open("array.x", O_WRONLY | O_CREAT | O_APPEND | O_TRUNC),
	    .fd_type = SLSMM_FD_FILE,
	};
	int pid = fork();
	printf("%d\n", getpid());
	if (pid == 0) {
	    ioctl(sls, FULL_DUMP, &param);
	    exit(0);
	} else {
	    waitpid(pid, 0, 0);
	}
}

int main() {
	int sls = open("/dev/slsmm", O_RDWR);
	printf("%d\n", getpid());
	memset(arr, 0x3f, sizeof(arr));
	printf("%x\n", (unsigned int)arr);
	for (int i = 0; i < 100; i ++) {
		sleep(1);
		printf("%d %x\n", i, arr[i%128]);
		full_dump_self(sls);
	}
	return 0;
}
