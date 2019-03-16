#ifndef _DESC_H_
#define _DESC_H_


enum descriptor_type {
    DESC_FD,
//    DESC_MD,
    DESC_OSD,
    DESC_FILE,
    DESCRIPTOR_SIZE,
};

#define SLS_POISON (1UL) 

struct sls_desc{
    enum descriptor_type type; 
    union {
	uintptr_t index;
	int desc;
    };
};

struct sls_desc create_desc(long index, int type);
void destroy_desc(struct sls_desc desc);

#endif /* _DESC_H_ */
