#ifndef __BUFC_H__
#define __BUFC_H__

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/condvar.h>

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

chan_t* chan_init(size_t capacity);
void chan_send(chan_t* chan, void* data);
void* chan_recv(chan_t* chan);

#endif
