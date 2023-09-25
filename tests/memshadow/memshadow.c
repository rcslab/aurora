#include <sys/param.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <assert.h>
#include <sls.h>
#include <sls_ioctl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>

#define WRITE_REGION (1024 * 1024 * 64)
#define OID (1536)
#define ROUNDS (20)

int *region;

int
produce_flts(int seed)
{
	struct rusage ru;
	int error;
	int i;

	for (i = 0; i < WRITE_REGION / PAGE_SIZE; i++) {
		if (seed > 0) {
			assert(region[i * (PAGE_SIZE / sizeof(int))] ==
			    seed - 1 + i);
		}
		region[i * (PAGE_SIZE / sizeof(int))] = seed + i;
	}

	error = getrusage(RUSAGE_SELF, &ru);
	if (error != 0) {
		fprintf(stderr, "%s: %d error %d\n", __func__, __LINE__, error);
		exit(EX_SOFTWARE);
	}

	return ru.ru_minflt;
}

int
main(int argc, char *argv[])
{
	int flts_old, flts_new, flts_expected = 0;
	struct sls_attr attr;
	struct rusage ru;
	int rounds;
	int error;

	attr = (struct sls_attr) {
		.attr_target = SLS_OSD,
		.attr_mode = SLS_DELTA,
		.attr_period = 0,
		.attr_flags = SLSATTR_IGNUNLINKED,
		.attr_amplification = 1,
	};

	region = mmap((void *)0x1200000000, WRITE_REGION,
	    PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (region == NULL) {
		fprintf(stderr, "%s: %d\n", __func__, __LINE__);
		exit(EX_SOFTWARE);
	}
	memset(region, 0, WRITE_REGION);

	error = sls_partadd(OID, attr, -1);
	if (error != 0) {
		fprintf(stderr, "%s: %d\n", __func__, __LINE__);
		exit(EX_OSERR);
	}

	error = sls_attach(OID, getpid());
	if (error != 0) {
		fprintf(stderr, "%s: %d\n", __func__, __LINE__);
		exit(EX_OSERR);
	}

	error = sls_checkpoint(OID, true);
	if (error != 0) {
		fprintf(stderr, "%s: %d\n", __func__, __LINE__);
		exit(EX_OSERR);
	}

	/* Checkpoint twice to create an initial delta. */
	error = sls_checkpoint(OID, true);
	if (error != 0) {
		fprintf(stderr, "%s: %d\n", __func__, __LINE__);
		exit(EX_OSERR);
	}

	error = getrusage(RUSAGE_SELF, &ru);
	if (error != 0) {
		fprintf(stderr, "%s: %d error %d\n", __func__, __LINE__, error);
		exit(EX_SOFTWARE);
	}

	flts_old = ru.ru_minflt;
	for (rounds = 0; rounds < ROUNDS + 1; rounds++) {
		error = sls_memsnap(OID, region);
		if (error != 0) {
			fprintf(stderr, "%s: %d\n", __func__, __LINE__);
			exit(EX_OSERR);
		}

		flts_new = produce_flts(rounds);

		if (rounds <= 3) {
			flts_old = flts_new;
			continue;
		}

		if (flts_expected == 0)
			flts_expected = flts_new - flts_old;

		assert(flts_new - flts_old == flts_expected);
		flts_old = flts_new;
	}

	return (0);
}
