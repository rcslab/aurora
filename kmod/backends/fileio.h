#ifndef _FILEIO_H_
#define _FILEIO_H_

#include <sys/types.h>

enum descriptor_type {
    DESC_FD,
//    DESC_MD,
    DESC_NVDIMM,
    DESCRIPTOR_SIZE,
};

#define SLS_POISON (1UL) 

struct sls_desc{
    enum descriptor_type type; 
    union {
	void *index;
	int desc;
    };
};

struct sls_desc create_desc(long index, int type);
void destroy_desc(struct sls_desc desc);

int fd_read(void* addr, size_t len, struct sls_desc desc);
int fd_write(void* addr, size_t len, struct sls_desc desc);

void backends_init(void);
void backends_fini(void);

#endif
