#ifndef _SLS_METROPOLIS_H_
#define _SLS_METROPOLIS_H_

struct slsmetr {
	uint64_t slsmetr_proc; /* SLS ID of process to accept */
	uint64_t slsmetr_td;   /* SLS ID of accepting thread */
	void *slsmetr_addrsa;  /* Userspace pointer to the socket address */
	void *slsmetr_addrlen; /* Userspace pointer to the socket length */
	struct file *slsmetr_sockfp; /* File pointer to connected socket */
	struct sockaddr *slsmetr_sa; /* Address of the connected socket */
	socklen_t slsmetr_namelen;   /* Size of the above address */
	uint64_t slsmetr_sockid;     /* Socket ID of the problematic socket. */
	int slsmetr_flags;	     /* flags for accept4() */
};

/* The Aurora syscall vector. */
extern struct sysentvec slsmetropolis_sysvec;

void slsmetropolis_initsysvec(void);
void slsmetropolis_finisysvec(void);

#endif /* _SLS_METROPOLIS_H_ */
