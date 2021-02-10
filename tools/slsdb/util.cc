
#include <ctime>
#include <string>

std::string
time_to_string(uint64_t sec, uint64_t nsec)
{
	time_t timesec = sec;
	struct tm ltime;
	char buf[64];
	std::string rval;

	if (localtime_r(&timesec, &ltime) == NULL) {
		perror("localtime_r");
		return "";
	}

	strftime(buf, sizeof(buf), "%F %T", &ltime);

	snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ".%09lu", nsec);

	rval = buf;

	return rval;
}

void
hexdump(const char *data, size_t length, off_t off)
{
	const size_t row_size = 16;
	bool stop = false;

	for (size_t row = 0; !stop; row++) {
		size_t ix = row * row_size;
		if (ix >= length) {
			break;
		}

		printf("%08lx  ", row * row_size + off);
		for (size_t col = 0; col < row_size; col++) {
			size_t ix = row * row_size + col;
			if (length != 0 && ix > length) {
				stop = true;
				for (; col < row_size; col++) {
					printf("   ");
				}
				break;
			}

			printf("%02X ", (unsigned char)data[ix]);
		}
		printf("  |");

		for (size_t col = 0; col < row_size; col++) {
			size_t ix = row * row_size + col;
			if (length != 0 && ix > length) {
				stop = true;
				for (; col < row_size; col++) {
					printf(" ");
				}
				break;
			}

			unsigned char c = (unsigned char)data[ix];
			if (c >= 0x20 && c < 0x7F)
				printf("%c", c);
			else
				putchar('.');
		}
		printf("|");
		printf("\n");
	}
}
