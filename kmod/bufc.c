#include "bufc.h"
#include "_slsmm.h"

chan_t*
chan_init(size_t capacity)
{
    chan_t* chan = (chan_t*)malloc(sizeof(chan_t), M_SLSMM, M_NOWAIT);
    if (!chan)
	return NULL;

    chan->data = malloc(capacity * sizeof(void*), M_SLSMM, M_NOWAIT);
    if (!chan->data) {
	free(chan, M_SLSMM);
	return NULL;
    }

    chan->size = 0;
    chan->w_wait = 0;
    chan->r_wait = 0;
    chan->head = 0;
    chan->tail = 0;
    chan->capacity = capacity;

    cv_init(&chan->w_cv, "chan w_cv"); 
    cv_init(&chan->r_cv, "chan r_cv"); 
    mtx_init(&chan->mu, "chan mutex", NULL, MTX_DEF);
    return chan;
}

void 
chan_send(chan_t* chan, void* data)
{
    mtx_lock(&chan->mu);
    while (chan->size == chan->capacity) {
	chan->w_wait ++;
	cv_wait(&chan->w_cv, &chan->mu);
	chan->w_wait --;
    }
    
    chan->data[chan->tail] = data;
    chan->size ++;
    chan->tail = (chan->tail + 1) % chan->capacity;

    if (chan->r_wait)
	cv_signal(&chan->r_cv);

    mtx_unlock(&chan->mu);
}

void* 
chan_recv(chan_t* chan)
{
    void* data;
    mtx_lock(&chan->mu);

    while (chan->size == 0) {
	chan->r_wait ++;
	cv_wait(&chan->r_cv, &chan->mu);
	chan->r_wait --;
    }

    data = chan->data[chan->head];
    chan->size --;
    chan->head = (chan->head + 1) % chan->capacity;

    if (chan->w_wait)
	cv_signal(&chan->w_cv);

    mtx_unlock(&chan->mu);
    return data;
}
