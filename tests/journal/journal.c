#include <sys/types.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

double
subtract(struct timespec a, struct timespec b)
{
	uint64_t s = (a.tv_sec - b.tv_sec);
	if (a.tv_nsec < b.tv_nsec) {
		a.tv_nsec += 1000000000;
		s -= 1;
	}
	double ns_u = (float)(a.tv_nsec - b.tv_nsec) / 1000.0;
	double convert = s * 1000000;
	convert += ns_u;
	return convert;
}

double
do_one(int fd, char *buf, int sz)
{
	struct timespec sp1;
	struct timespec sp2;

	int error = clock_gettime(CLOCK_REALTIME_FAST, &sp1);
	if (error) {
		perror("clock_gettime (1)");
		return -1;
	}

	if (sz)
		pwrite(fd, buf, sz, 0L);

	error = clock_gettime(CLOCK_REALTIME_FAST, &sp2);
	if (error) {
		perror("clock_gettime (2)");
		return -1;
	}
	double time = subtract(sp2, sp1);

	return time;
}

int
main(int argc, char *argv[])
{
	if (argc != 3) {
		printf("./main <DISK> <SIZE (in bytes)>\n");
		return -1;
	}
	char *ptr;
	/* Create an in-memory buffer to be flushed. */
	long int size = strtol(argv[2], &ptr, 10);
	char *buf = malloc(size);
	int fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		perror("open");
		return -1;
	}

#define NSAMPLES 5
	double samples[NSAMPLES];
	double baseline;

	/* Get the baseline latency. */
	for (int i = 0; i < NSAMPLES; i++)
		baseline += do_one(fd, buf, 0);
	baseline /= NSAMPLES;

	/* Get the average latency of the sample minus the baseline. */
	for (int i = 0; i < NSAMPLES; i++)
		samples[i] = do_one(fd, buf, size) - baseline;

	double avg = 0;
	for (int i = 1; i < NSAMPLES; i++)
		avg += samples[i];

	avg /= (NSAMPLES - 1);

	printf("%.0fus\n", avg);

	return (0);
}
