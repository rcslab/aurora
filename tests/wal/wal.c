#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sls.h>
#include <sls_wal.h>

int
main()
{
	struct sls_wal wal;
	int forty_two = 42, twenty_four = 24;
	int value;

	if (sls_wal_open(&wal, SLS_DEFAULT_PARTITION, 4096) != 0) {
		perror("sls_wal_open()");
		return (EXIT_FAILURE);
	}

	sls_wal_memcpy(&wal, &value, &twenty_four, sizeof(value));
	printf("value: %d\n", value);

	if (sls_wal_savepoint(&wal) != 0) {
		perror("sls_wal_sync()");
		return (EXIT_FAILURE);
	}

	value = 0;
	sls_wal_replay(&wal);
	printf("value: %d\n", value);

	if (value == 42) {
		goto done;
	} else if (value != 24) {
		fprintf(stderr, "Unexpected value %d\n", value);
		return (EXIT_FAILURE);
	}

	sls_wal_memcpy(&wal, &value, &forty_two, sizeof(value));
	printf("value: %d\n", value);

	if (sls_wal_sync(&wal) != 0) {
		perror("sls_wal_sync()");
		return (EXIT_FAILURE);
	}

done:
	if (sls_wal_close(&wal) != 0) {
		perror("sls_wal_close()");
		return (EXIT_FAILURE);
	}

	return (value == 42 ? EXIT_SUCCESS : EXIT_FAILURE);
}
