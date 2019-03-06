#ifndef __BUFC_H__
#define __BUFC_H__

#include <sys/param.h>

#include <sys/capsicum.h>
#include <sys/condvar.h>
#include <sys/lock.h>
#include <sys/mutex.h>

#include <slsmm.h>

typedef struct chan_t {
	void** data;

	size_t capacity;
	size_t size;
	size_t head, tail;
	size_t w_wait, r_wait;

	struct mtx mu;
	struct cv w_cv;
	struct cv r_cv;
} chan_t;

struct sls_chan_args {
    struct proc *p;
    int dump_mode;
    char *filename;
    int request_id;
    int fd_type;
};

chan_t* chan_init(size_t capacity);
void chan_send(chan_t* chan, void* data);
void* chan_recv(chan_t* chan);

#endif
