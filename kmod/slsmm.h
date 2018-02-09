#ifndef __SLSMM_H__
#define __SLSMM_H__

#include <sys/param.h>
#include <sys/cdefs.h>
#include <sys/ioccom.h>
#include <sys/malloc.h>
#include <sys/kernel.h>

#define SLSMM_READ  _IOR('t', 1, char)
#define SLSMM_WRITE _IOW('t', 2, char)
#define SLSMM_PAGE_FLAGS _IO('t', 3)

struct slsmm_page;
struct slsmm_object;
typedef struct slsmm_page slsmm_page_t;
typedef struct slsmm_object slsmm_object_t;

struct slsmm_page {
    vm_page_t page;
    int device;
};

struct slsmm_object {
    struct cdev *dev;
    slsmm_page_t *pages;
    size_t size;
};

MALLOC_DECLARE(M_SLSMM);

#endif
