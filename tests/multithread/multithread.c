#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <pthread.h>

/* Constants */
#define NUMTHREADS (5)
#define SLEEPTIME (1)
#define CYCLES (100 * 1000 * 1000)

/* 
 * Each value causes a different action in the 
 * thread function.
 */
enum action {
	NOTHING,
	SLEEP,
	COMPUTE,
	PRINT,
	NUM_ACTIONS,
};

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


/* 
 * Exists solely to confuse -O2 compilation and
 * prevent the computational loop from being 
 * optimized out.
 */ 
int compute_var[NUMTHREADS]; 

int payload(enum action payload_action, void *data)
{
	int i = 0;
	long long threadno;

	threadno = (long long) data;
	
	switch(payload_action) {

	case NOTHING:
		break;

	case SLEEP:
		sleep(SLEEPTIME);
		break;


	case COMPUTE:
		/* Totally bogus computation */
		for (i = 0; i < CYCLES; i++)
			compute_var[threadno] = (compute_var[threadno]+ 127) % 37;
		break;

	case PRINT:
		printf("Thread %lld printing %d\n", threadno , compute_var[threadno]);
		break;

	default:
		printf("ERROR: INVALID PAYLOAD\n");
		exit(1);
	}

	return i;
}


void *thread_func(void *data)
{

	for (;;) {
		pthread_mutex_lock(&mutex);
		payload(COMPUTE, data);
		payload(PRINT, data);
		pthread_mutex_unlock(&mutex);
		payload(COMPUTE, data);
	}

	pthread_exit(NULL);
}


int main()
{
	pthread_t threads[NUMTHREADS];
	long long i;


	for (i = 0; i < NUMTHREADS; i++)
		pthread_create(&threads[i], NULL, thread_func, (void *) i);

	pthread_exit(NULL);
}
