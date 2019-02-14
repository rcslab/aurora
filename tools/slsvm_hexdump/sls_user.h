#ifndef _SLS_USER_H
#define _SLS_USER_H

#include "../kmod/dump.h"

struct slspage {
	vm_offset_t vaddr;
	uint8_t data[PAGE_SIZE];
	struct slspage *next;
};

struct dump *sls_load_dump(FILE *fp);


#endif /* _SLS_USER_H */
