#include <sys/param.h>
#include <sys/endian.h>
#include <sys/queue.h>

#include <machine/param.h>

#include <sys/domain.h>
#include <sys/event.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/mman.h>
#include <sys/mutex.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/protosw.h>
#include <sys/rwlock.h>
#include <sys/sbuf.h>
#include <sys/selinfo.h>
#include <sys/shm.h>
#include <sys/socketvar.h>
#include <sys/stat.h>
#include <sys/syscallsubr.h>
#include <sys/unistd.h>
#include <sys/vnode.h>

/* 
 * XXX eventvar should include more headers,
 * it can't be placed alphabetically.
 */
#include <sys/eventvar.h>

#include <netinet/in.h>
#include <netinet/in_pcb.h>

#include <vm/pmap.h>
#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_radix.h>
#include <vm/uma.h>

#include <slos.h>
#include <sls_data.h>

#include "sls_file.h"
#include "sls_internal.h"
#include "sls_mm.h"
#include "sls_path.h"

#include "imported_sls.h"

/* Checkpoint a kqueue and all of its pending knotes. */
int
slsckpt_kqueue(struct proc *p, struct kqueue *kq, struct sbuf *sb)
{
	struct slskqueue kqinfo;
	struct slskevent kevinfo;
	struct knote *kn;
	int error, i;

	/* Create the structure for the kqueue itself. */
	kqinfo.magic = SLSKQUEUE_ID;
	kqinfo.slsid = (uint64_t) kq;

	/* Write the kqueue itself to the sbuf. */
	error = sbuf_bcat(sb, (void *) &kqinfo, sizeof(kqinfo));
	if (error != 0)
	    return (error);


	/* Traverse all lists of knotes in the kqueue. */
	for (i = 0; i < kq->kq_knlistsize; i++) {
	    /* Get all knotes from each list. */
	    SLIST_FOREACH(kn, &kq->kq_knlist[i], kn_link) {
		/* 
		 * XXX Check whether any knotes are in flux. 
		 * This is erring on the side of caution, since
		 * I'm not sure what being in flux would do for us.
		 */
		if (kn != NULL && (kn->kn_influx > 0))
		    panic("knote in flux while checkpointing");
		
		/* 
		 * 
		 * Get all relevant fields. Most of them are 
		 * from the kevent struct the knote points to.
		 */
		kevinfo.status = kn->kn_status;
		kevinfo.ident = kn->kn_kevent.ident;
		kevinfo.filter = kn->kn_kevent.filter;
		kevinfo.flags = kn->kn_kevent.flags;
		kevinfo.fflags = kn->kn_kevent.fflags;
		kevinfo.data = kn->kn_kevent.data;
		kevinfo.slsid= (uint64_t) kn;
		kevinfo.magic = SLSKEVENT_ID;

		/* Write each kevent to the sbuf. */
		error = sbuf_bcat(sb, (void *) &kevinfo, sizeof(kevinfo));
		if (error != 0)
		    return (error);
	    }
	}

	return (0);
}

/*
 * Restore a kqueue for a process. Kqueues are an exception among
 * entities using the file abstraction, in that they need the rest
 * of the files in the process' table to be restored before we can
 * fully restore it. More specifically, while we create the kqueue
 * here, in order for it to be inserted to the proper place in the 
 * file table, we do not populate it with kevents. This is because
 * each kevent targets an fd, and so it can't be done while the
 * files are being restored. We wait until that step is done before
 * we fully populate the kqueues with their data.
 */
int
slsrest_kqueue(struct slskqueue *kqinfo, int *fdp)
{
	int error;

	/* Create a kqueue for the process. */
	error = kern_kqueue(curthread, 0, NULL);
	if (error != 0)
	    return (error);

	/* Grab the open file and pass it to the caller. */
	*fdp = curthread->td_retval[0];

	return (0);
}


/*
 * Restore the kevents of a kqueue. This is done after all files
 * have been restored, since we need the fds to be backed before
 * we attach the relevant kevent to the kqueue.
 */
int
slsrest_kevents(int fd, slsset *slskevs)
{
	struct slskevent *slskev;
	struct kevent *kev;
	int error;

	/* For each kqinfo, create a kevent and register it. */
	KVSET_FOREACH_POP(slskevs, slskev) {
	    kev = malloc(sizeof(*kev), M_SLSMM, M_WAITOK);

	    kev->ident = slskev->ident;
	    /* XXX Right now we only need EVFILT_READ, which we 
	    * aren't getting at checkpoint time. Find out why.
	    */
	    kev->filter = slskev->filter; 
	    kev->flags = slskev->flags;
	    kev->fflags = slskev->fflags;
	    kev->data = slskev->data;
	    /* Add the kevent to the kqueue */
	    kev->flags = EV_ADD;

	    /* If any of the events were disabled , keep them that way. */
	    if ((slskev->status & KN_DISABLED) != 0)
		kev->flags |= EV_DISABLE;

	    printf("Registering kevent for fd %d\n", fd);
	    error = kqfd_register(fd, kev, curthread, 1);
	    if (error != 0) {
		/* 
		* We don't handle restoring certain types of
		* fds like IPv6 sockets yet.
		*/
		SLS_DBG("(BUG) fd for kevent %ld not restored\n", kev->ident);
	    }
	}

	return (0);
}
