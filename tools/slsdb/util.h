
#ifndef __UTIL_H__
#define __UTIL_H__

#define SECTOR_SIZE 512
#define ROUNDUP(a, b) ((((a) + (b - 1)) / (b)) * (b))

std::string time_to_string(uint64_t sec, uint64_t nsec);
void hexdump(const char *data, size_t length, off_t off);

#endif /* __UTIL_H__ */
